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

#ifndef _GC_RideCache_h
#define _GC_RideCache_h 1

#include "GoldenCheetah.h"
#include "MainWindow.h"
#include "RideFile.h"
#include "RideItem.h"
#include "RideCachePersistence.h"
#include "RideCacheStartup.h"
#include "PDModel.h"

#include <atomic>
#include <functional>
#include <memory>

#include <QVector>
#include <QThread>
#include <QPointer>
#include <QSemaphore>
#include <QMultiHash>
#include <QHash>
#include <QSet>

#include <QFuture>
#include <QFutureWatcher>
# include <QtConcurrent>

class Context;
class LTMPlot;
class RideCacheRefreshThread;
class RideCacheLoader;
class RideCacheBackgroundSaver;
class Specification;
class RideCacheSnapshotBatch;
class AthleteBest;
class RideCacheModel;
class Estimator;
class Banister;
class RideCacheMutationScope;

namespace RideCacheSave {
struct Snapshot;
}

class RideCache : public QObject
{
    Q_OBJECT

    public:
        struct OperationResult;
        struct RemovalResult;


        RideCache(Context *context);
        ~RideCache();

        // table models
        RideCacheModel *model() { return model_; }

        // query the cache
        int count() const {
            if (deletelist.isEmpty()) return rides_.count();

            int liveCount = 0;
            for (RideItem *item : rides_) {
                if (item && !deletelist.contains(item)) ++liveCount;
            }
            return liveCount;
        }
        RideItem *getRide(QString filename);
        RideItem *getRide(const QString &filename, bool planned);
        RideItem *getRide(QDateTime dateTime);
	    QList<QDateTime> getAllDates();
        QStringList getAllFilenames();

        // get an aggregate applying the passed spec
        QString getAggregate(QString name, Specification spec, bool useMetricUnits, bool nofmt=false);
        QVector<QStringList> getAggregates(
            const QStringList &names,
            const QVector<Specification> &specifications,
            bool useMetricUnits,
            bool nofmt=false);

        // get top n bests
        QList<AthleteBest> getBests(QString symbol, int n, Specification specification, bool useMetricUnits=true);

        // metadata
        QHash<QString,int> getRankedValues(QString name); // metadata
        QStringList getDistinctValues(QString name); // metadata

        // Count of activities matching specification
        void getRideTypeCounts(Specification specification, int& nActivities,
                               int& nRides, int& nRuns, int& nSwims, QString& sport);
        // Check if metric is relevant for some  activity matching specification
        enum SportRestriction { AnySport, OnlyRides, OnlyRuns, OnlySwims, OnlyXtrains };
        bool isMetricRelevantForRides(Specification specification,
                                      const RideMetric* metric,
                                      SportRestriction sport=AnySport);
        QVector<bool> areMetricsRelevantForRides(
            const QVector<Specification> &specifications,
            const QVector<const RideMetric*> &metrics,
            SportRestriction sport=AnySport);

        // is running ?
        bool isRunning() { return refreshThreads.count() != 0; }
        bool isStartupLoadFinished() const {
            return startupLoadFinished_;
        }
        QString startupRecoveryError() const {
            return startupRecoveryError_;
        }

        // how is update going?
        QMutex updateMutex;
        int updates; // for watching progress
        int nextRefresh(quint64 generation);
        void threadCompleted(RideCacheRefreshThread*, quint64 generation);

        // the ride list
        QVector<RideItem*> rides() const {
            if (deletelist.isEmpty()) return rides_;

            QVector<RideItem*> live;
            live.reserve(rides_.size());
            for (RideItem *item : rides_) {
                if (item && !deletelist.contains(item)) live.append(item);
            }
            return live;
        }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        QVector<RideItem*> &mutableRidesForRemovalTest() { return rides_; }
        qsizetype pendingDeletionCountForRemovalTest() const {
            return delete_.size();
        }
        void discardPendingDeletionAddressForRemovalTest(
            RideItem *address) {
            delete_.removeOne(address);
        }
        bool remembersDeletedAddressForRemovalTest(
            RideItem *address) const {
            return deletelist.contains(address);
        }
        void markDeletedAddressForRemovalTest(
            RideItem *address) {
            deletelist.insert(address);
        }
        bool replacementOperationInProgressForTest() const {
            return removalInProgress_ && *removalInProgress_;
        }
        bool activityMutationBlockedForTest() const {
            return activityMutationIsBlocked();
        }
#endif

        // add/remove a ride to the list
        void addRide(QString name, bool dosignal, bool select, bool useTempActivities, bool planned);
        // Takes ownership of each non-null prepared ride.
        QVector<RideItem*> addRides(
            const QStringList &names,
            const QVector<RideFile*> &preparedRides,
            bool dosignal,
            bool select,
            bool useTempActivities,
            bool planned);
        bool removeCurrentRide();
        RemovalResult removeCurrentRideResult();
        bool removeRide(const QString& filenameToDelete);
        RemovalResult removeRideResult(
            const QString &filenameToDelete);
        bool removeRide(
            const QString &filenameToDelete,
            bool planned);
        RemovalResult removeRideResult(
            const QString &filenameToDelete,
            bool planned);
        bool removeRide(RideItem *rideToDelete);
        RemovalResult removeRideResult(
            RideItem *rideToDelete);
        bool removeArchivedRide(const QString& filenameToDelete);
        RemovalResult removeArchivedRideResult(
            const QString &filenameToDelete);
        bool removeArchivedRide(RideItem *rideToDelete);
        RemovalResult removeArchivedRideResult(
            RideItem *rideToDelete);
        bool removeRides(const QStringList &filenamesToDelete, bool triggerRefresh = true);
        RemovalResult removeRidesResult(
            const QStringList &filenamesToDelete,
            bool triggerRefresh = true);
        bool removeRides(const QList<RideItem*> &ridesToDelete, bool triggerRefresh = true);
        RemovalResult removeRidesResult(
            const QList<RideItem*> &ridesToDelete,
            bool triggerRefresh = true);
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        bool renameRideFilesForTest(
            const QString &oldFileName,
            const QString &newFileName,
            bool isPlanned,
            QString &error)
        {
            return renameRideFiles(
                oldFileName,
                newFileName,
                isPlanned,
                error);
        }
#endif

        // export metrics in CSV format
        void writeAsCSV(QString filename);

        // the background refresher !
        void refresh();
        double progress() { return progress_; }

        struct OperationPreCheck {
            bool canProceed = true;
            QString blockingReason;
            QList<RideItem*> affectedItems; // All items that would be modified
            QList<RideItem*> dirtyItems; // Affected items that are already dirty
            bool requiresUserDecision = false;
            QString warningMessage;
        };

        struct OperationResult {
            bool success = false;
            QString error;
            int affectedCount = 0;
        };

        struct PlannedActivityTarget {
            QString fileName;
            std::function<bool(
                const QString &stagingPath,
                QString &error)> stage;
        };

        struct PlannedReplacementResult {
            bool committed = false;
            bool cacheUpdated = false;
            bool cleanupComplete = false;
            QString error;
            int removedCount = 0;
            int addedCount = 0;
            QStringList warnings;

            bool cleanlyCompleted() const
            {
                return committed && cacheUpdated
                    && cleanupComplete && error.isEmpty();
            }
        };

        enum class RemovalStatus {
            Rejected,
            RolledBack,
            Committed,
            CommittedCleanupPending,
            PartiallyCommitted,
            RecoveryRequired,
            NotAttempted
        };

        struct RemovalItemResult {
            QString fileName;
            bool planned = false;
            RemovalStatus status = RemovalStatus::Rejected;
            QString error;
            QStringList recoveryPaths;

            bool logicallyRemoved() const
            {
                return status == RemovalStatus::Committed
                    || status
                        == RemovalStatus::CommittedCleanupPending;
            }
        };

        struct RemovalResult {
            RemovalStatus status = RemovalStatus::Rejected;
            QString error;
            int requestedCount = 0;
            int affectedCount = 0;
            QStringList recoveryPaths;
            QVector<RemovalItemResult> items;

            bool cleanlyCompleted() const
            {
                return status == RemovalStatus::Committed;
            }

            bool allLogicallyRemoved() const
            {
                return requestedCount > 0
                    && affectedCount == requestedCount
                    && (status == RemovalStatus::Committed
                        || status
                            == RemovalStatus::CommittedCleanupPending);
            }

            bool anyLogicallyRemoved() const
            {
                return affectedCount > 0;
            }

            bool requiresRecovery() const
            {
                return status
                    == RemovalStatus::RecoveryRequired;
            }
        };

        // Split validations out of the action-methods to allow user interaction if dependent
        // activities need to be saved or reverted.
        // (!) The action-methods don't repeat the input-validation, check is always required upfront

        OperationPreCheck checkLinkActivities(RideItem *item1, RideItem *item2);
        OperationResult linkActivities(RideItem *item1, RideItem *item2);

        OperationPreCheck checkUnlinkActivity(RideItem *item);
        OperationResult unlinkActivity(RideItem *item);

        OperationPreCheck checkRemovalLinks(RideItem *item);

        OperationPreCheck checkUnlinkActivities(const QList<RideItem*> &items);
        OperationResult unlinkActivities(const QList<RideItem*> &items);

        OperationPreCheck checkMoveActivity(RideItem *item, const QDateTime &newDateTime);
        OperationResult moveActivity(RideItem *item, const QDateTime &newDateTime);

        OperationPreCheck checkCopyPlannedActivity(RideItem *sourceItem, const QDate &newDate, QTime newTime = QTime());
        OperationResult copyPlannedActivity(RideItem *sourceItem, const QDate &newDate, QTime newTime = QTime());

        OperationPreCheck checkCopyPlannedActivities(const QList<std::pair<RideItem*, QDate>> &sourceItemsAndTargets);
        OperationResult copyPlannedActivities(const QList<std::pair<RideItem*, QDate>> &sourceItemsAndTargets);
        PlannedReplacementResult replacePlannedActivityFiles(
            const QList<RideItem*> &activitiesToReplace,
            const QStringList &inputPaths,
            const QList<PlannedActivityTarget> &targets,
            bool notifyAdded = false);
        PlannedReplacementResult replacePlannedActivities(
            const QList<RideItem*> &activitiesToReplace,
            const QList<std::pair<RideItem*, QDate>>
                &sourceItemsAndTargets);

        OperationPreCheck checkShiftPlannedActivities(const QDate &fromDate, int dayOffset);
        OperationResult shiftPlannedActivities(const QDate &fromDate, int dayOffset);

        using SaveActivityFunction =
            std::function<bool(Context *, RideItem *, QString *)>;
        using ActivitySavedFunction = std::function<void(RideItem *)>;

        bool saveActivity(RideItem *item, QString &error);
        bool saveActivity(
            RideItem *item,
            QString &error,
            const AtomicFileWriterFactory &writerFactory);
        bool saveActivities(QList<RideItem*> items, QString &error);
        static bool saveActivity(
            Context *context, RideItem *item, QString &error,
            const SaveActivityFunction &save,
            const ActivitySavedFunction &notifySaved = ActivitySavedFunction(),
            QObject *operationOwner = nullptr);
        static bool saveActivities(
            Context *context, const QList<RideItem *> &items, QString &error,
            const SaveActivityFunction &save,
            const ActivitySavedFunction &notifySaved = ActivitySavedFunction(),
            QObject *operationOwner = nullptr);

        RideItem *getLinkedActivity(RideItem *item);
        RideItem *findSuggestion(RideItem *rideItem);

        bool updateFromWorkout(RideItem *item, bool autoSave = false);
        bool updateFromWorkoutAfter(const QDate &when, bool autoSave = false);

        bool saveToFile(
            bool opendata, const QString &filename, QString &error,
            const AtomicFileWriterFactory &writerFactory =
                qSaveFileWriterFactory());
        bool settleForOpenDataSnapshot(QString &error);

    public slots:

        // restore / dump cache to disk (json)
        void load();
        void postLoad();
        void save(bool opendata=false, QString filename="");

        // Kept for test/link compatibility; completion is generation-aware.
        void cleanupThread(RideCacheRefreshThread *thread);

        // find entry quickly
        int find(RideItem *);

        // user updated options/preferences
        void configChanged(qint32);

        // background refresh progress update
        void progressing(int);

        // cancel background processing because about to exit
        void cancel();

        // item telling us it changed
        void itemChanged();

        // clear deleted objects
        void garbageCollect();

        // first run to initialise estimates
        void initEstimates();

    signals:

        void modelProgress(int, int); // let others know when we're refreshing the model estimates
        void loadComplete(); // the file index and model are ready
        void startupLoadFinished(); // persisted metadata has been restored

        // us telling the world the item changed
        void itemChanged(RideItem*);
        void itemSaved(RideItem *item);

    protected:

        friend class ::Athlete;
        friend class ::MainWindow; // save dialog
        friend class ::LTMPlot; // get weekly performances
        friend class ::Banister; // get weekly performances
        friend class ::Leaf; // get weekly performances
        friend class ::RideItem; // adds to deletelist in destructor
        friend class ::RideCacheRefreshThread;
        friend class ::RideCacheModel;
        friend class ::RideCacheMutationScope;

        friend class ::RideCacheLoader;
        Context *context;
        QDir directory, plannedDirectory;

        // rides and reverse are the main lists
        // delete_ is a list of items to garbage collect (delete later)
        // deletelist contains transient tombstones for destroyed cache entries.
        QVector<RideItem*> rides_, reverse_, delete_;
        QSet<RideItem*> deletelist;
        int modelRowCount() const;
        RideItem *modelRideAt(int row) const;
        int modelIndexOf(RideItem *item) const;
        RideCacheModel *model_ = nullptr;
        bool exiting = false;
	    double progress_; // percent

        QVector<RideCacheRefreshThread*> refreshThreads;

        Estimator *estimator;
        bool first; // updated when estimates are marked stale

    private:
        bool activityMutationIsBlocked() const;
        bool ownsLiveRide(const RideItem *item) const {
            RideItem *const address = const_cast<RideItem*>(item);
            return address
                && !deletelist.contains(address)
                && rides_.contains(address);
        }

        enum class RideFileDisposition {
            Archive,
            AlreadyArchived
        };

        void appendStartupFiles(
            const std::shared_ptr<
                QVector<RideCacheStartup::IndexedFile>> &files);
        void startupIndexComplete();
        bool queueStartupSnapshots(
            const std::shared_ptr<RideCacheSnapshotBatch> &batch);
        void applyStartupSnapshots(
            const std::shared_ptr<RideCacheSnapshotBatch> &batch);
        RideItem *startupItemFor(
            const QString &fileName,
            const QDateTime &dateTime) const;
        void invalidateStartupSnapshots();
        QStringList startupRideFiles(const QDir &directory) const;

        void startLatestRefresh();
        void interruptActiveRefresh();
        bool settleRefreshForSave(QString &error);
        std::shared_ptr<const RideCacheSave::Snapshot>
            captureSaveSnapshot(const QString &targetPath);
        bool enqueueSaveSnapshot(
            const std::shared_ptr<
                const RideCacheSave::Snapshot> &snapshot);
        void queueBackgroundSave(
            const QString &targetPath = QString());

        RideItem *uniqueRideForFileName(
            const QString &fileName) const;
        RideItem *uniqueRideForIdentity(
            const QString &fileName,
            bool planned) const;
        RemovalResult removeRideEntry(
            RideItem *rideToDelete,
            RideFileDisposition disposition,
            bool triggerRefresh = true,
            bool workersQuiesced = false);
        bool purgeDestroyedModelRows();
        void purgeDestroyedRowsInsideModelReset();
        void discardDetachedTombstones();
        QString cpxCachePathForActivity(
            const QString &fileName,
            bool isPlanned) const;
        QStringList derivedFilePathsForRemoval(
            RideItem *rideToDelete) const;
        bool renameRideFiles(const QString& oldFileName, const QString& newFileName, bool isPlanned, QString &error);
        bool isValidLink(RideItem *item1, RideItem *item2, QString &error);
        RideItem* copyPlannedRideFile(RideItem *sourceItem, const QDate &newDate, const QTime &newTime, QString &error);
        bool stagePlannedActivityCopy(
            const QString &sourcePath,
            const QString &sourceFileName,
            const QDateTime &targetDateTime,
            const QString &stagingPath,
            QString &error);
        bool validatePlannedActivityStage(
            const QString &stagingPath,
            const QString &targetFileName,
            QString &error) const;
        RideFile *openPlannedActivityForDeleteProcessor(
            const QString &sourcePath,
            QString &error) const;

        RideCacheStartup::RefreshGeneration refreshGeneration_;
        QMultiHash<QString, RideItem*> startupItemsByFile_;
        QHash<RideItem*, int> startupRows_;
        std::atomic<qsizetype> startupExpectedRideCount_{0};
        QSemaphore startupSnapshotSlots_{
            RideCacheStartup::MaximumPendingSnapshotBatches};
        QThread *startupLoader_ = nullptr;
        bool startupIndexReady_ = false;
        bool startupLoadFinished_ = false;
        QString startupRecoveryError_;
        bool startupSnapshotsInvalidated_ = false;
        bool isCancelled = false;
        bool refreshChanged_ = false;
        bool refreshNotificationActive_ = false;
        bool saveSnapshotBoundary_ = false;
        std::shared_ptr<bool> removalInProgress_{
            std::make_shared<bool>(false)};
        bool removalRefreshPending_ = false;
        std::shared_ptr<bool> replacementRefreshBlocked_{
            std::make_shared<bool>(false)};
        quint64 mutationResumeGeneration_ = 0;
        QStringList pendingSaveTargets_;
        std::shared_ptr<RideCacheBackgroundSaver>
            backgroundSaver_;
};

class AthleteBest
{
    public:
    double nvalue;
    QString value; // formatted value
    QDate date;

    // for std::sort
    bool operator< (AthleteBest right) const { return (nvalue < right.nvalue); }
};

class RideCacheRefreshThread : public QThread
{
    friend class RideCache;

    public:
        RideCacheRefreshThread(
            RideCache *cache, quint64 generation);

    protected:

        // refresh metrics
        virtual void run() override;

    private:
        QPointer<RideCache> cache;
        quint64 generation;
};

#endif // _GC_RideCache_h
