/*
 * Copyright (c) 2009 Mark Liversedge (liversedge@gmail.com)
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

#ifndef _GC_SaveDialogs_h
#define _GC_SaveDialogs_h 1
#include "GoldenCheetah.h"

#include <QtGui>
#include <QDialog>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QTableWidget>
#include <functional>
#include <QHeaderView>

#include "AtomicFileWriter.h"
#include "RideCache.h"
#include "RideItem.h"
#include "Context.h"

class MainWindow;
class Athlete;

namespace ActivitySaveWorkflow {

struct Identity
{
    QString fileName;
    QString path;
    bool planned = false;
};

inline bool isCurrent(
    bool workflowAvailable,
    bool contextRequired,
    bool contextAvailable,
    bool athleteAvailable,
    bool cacheAvailable,
    bool relationshipsMatch,
    bool itemAvailable,
    bool membershipRequired,
    bool itemInCache,
    const Identity &expected,
    const Identity &current)
{
    if (!workflowAvailable || !itemAvailable
        || expected.fileName.isEmpty()
        || expected.path.isEmpty()
        || expected.fileName != current.fileName
        || expected.path != current.path
        || expected.planned != current.planned) {
        return false;
    }
    if (contextRequired
        && (!contextAvailable || !athleteAvailable
            || !cacheAvailable || !relationshipsMatch)) {
        return false;
    }
    return !membershipRequired || itemInCache;
}

} // namespace ActivitySaveWorkflow

namespace ActivityDeletionWorkflow {

struct Identity
{
    QString fileName;
    QString path;
    bool planned = false;
};

struct SavedIdentityEvidence
{
    bool itemWasSaved = false;
    Identity identity;
};

struct State
{
    bool workflowAvailable = false;
    bool viewAvailable = false;
    bool contextAvailable = false;
    bool athleteAvailable = false;
    bool cacheAvailable = false;
    bool itemAvailable = false;
    bool relationshipsMatch = false;
    bool itemInCache = false;
};

template<typename Workflow, typename View,
         typename ContextType, typename AthleteType,
         typename CacheType, typename RelationshipCheck>
State guardedOwnerState(
    const QPointer<Workflow> &workflow,
    const QPointer<View> &view,
    const QPointer<ContextType> &context,
    const QPointer<AthleteType> &athlete,
    const QPointer<CacheType> &cache,
    RelationshipCheck relationshipsMatch)
{
    State state;
    state.workflowAvailable = workflow;
    state.viewAvailable = view;
    state.contextAvailable = context;
    state.athleteAvailable = athlete;
    state.cacheAvailable = cache;
    if (workflow && view && context && athlete && cache) {
        state.relationshipsMatch = relationshipsMatch(
            workflow.data(), view.data(), context.data(),
            athlete.data(), cache.data());
    }
    return state;
}

inline bool ownersAreCurrent(const State &state)
{
    return state.workflowAvailable
        && state.viewAvailable
        && state.contextAvailable
        && state.athleteAvailable
        && state.cacheAvailable
        && state.relationshipsMatch;
}

inline bool identitiesMatch(
    const Identity &left, const Identity &right)
{
    return left.fileName == right.fileName
        && left.path == right.path
        && left.planned == right.planned;
}

inline bool isCurrent(
    const State &state,
    const Identity &expected,
    const Identity &current)
{
    return ownersAreCurrent(state)
        && state.itemAvailable
        && state.itemInCache
        && identitiesMatch(current, expected);
}

inline bool cacheContainsUniqueIdentity(
    RideCache *cache, RideItem *item,
    const Identity &identity)
{
    if (!cache || !item) return false;

    int matches = 0;
    for (RideItem *candidate : cache->rides()) {
        if (!candidate
            || candidate->fileName != identity.fileName
            || candidate->path != identity.path
            || candidate->planned != identity.planned) {
            continue;
        }
        ++matches;
        if (candidate != item) return false;
    }
    return matches == 1;
}

inline bool safeActivityFileName(const QString &fileName)
{
    return !fileName.isEmpty()
        && fileName != QStringLiteral(".")
        && fileName != QStringLiteral("..")
        && !fileName.contains(QLatin1Char('/'))
        && !fileName.contains(QLatin1Char('\\'))
        && !QDir::isAbsolutePath(fileName)
        && QFileInfo(fileName).fileName() == fileName;
}

inline bool adoptSavedIdentity(
    const State &state,
    const Identity &current,
    const SavedIdentityEvidence &evidence,
    const QString &cacheDirectory,
    Identity &expected)
{
    if (!ownersAreCurrent(state)
        || !state.itemAvailable
        || !state.itemInCache
        || !evidence.itemWasSaved
        || !identitiesMatch(current, evidence.identity)
        || current.planned != expected.planned
        || current.fileName == expected.fileName
        || !safeActivityFileName(expected.fileName)
        || !safeActivityFileName(current.fileName)) {
        return false;
    }

    const QString canonicalCache =
        QDir(cacheDirectory).canonicalPath();
    const QString canonicalExpected =
        QDir(expected.path).canonicalPath();
    const QString canonicalCurrent =
        QDir(current.path).canonicalPath();
    if (canonicalCache.isEmpty()
        || canonicalExpected != canonicalCache
        || canonicalCurrent != canonicalCache) {
        return false;
    }

    const QFileInfo savedFile(
        QDir(canonicalCache).filePath(current.fileName));
    if (!savedFile.exists() || !savedFile.isFile()
        || savedFile.isSymLink()
        || savedFile.absoluteDir().canonicalPath()
            != canonicalCache) {
        return false;
    }

    expected = current;
    return true;
}

} // namespace ActivityDeletionWorkflow

struct ActivitySaveOperations
{
    AtomicFileWriterFactory writerFactory;
    ActivitySaveStep finalize;
    ActivitySaveStep rollback;
    std::function<bool(RideFile *, QString &)> stage;
    std::function<void()> markClean;
    QDateTime timestamp;
    bool allowTargetReplacement = true;
    bool targetLockHeld = false;
    bool persistCompletesDurableTransaction = false;
};

bool saveActivityTransaction(Context *context, RideFile *ride,
                             const QString &targetPath,
                             const ActivitySaveOperations &operations,
                             QString &error);
using ActivityCandidateSave =
    std::function<bool(RideItem *candidate, QString &error)>;
bool saveActivityCandidate(RideItem *current, RideItem *candidate,
                           RideFile *replacement,
                           const ActivityCandidateSave &save,
                           QString &error);

class SaveSingleDialogWidget : public QDialog
{
    Q_OBJECT
    G_OBJECT


    public:
        SaveSingleDialogWidget(MainWindow *, Context *context, RideItem *);
        bool mayProceed() const { return mayProceed_; }

    public slots:
        void saveClicked();
        void abandonClicked();
        void cancelClicked();
        void warnSettingClicked();

    protected:
        virtual bool saveRide(QString &error);
        virtual void reportSaveError(const QString &error);

    private:
        bool activityIsCurrent() const;

        QPointer<Context> context;
        QPointer<Athlete> athlete;
        QPointer<RideCache> cache;
        QPointer<RideItem> rideItem;
        ActivitySaveWorkflow::Identity identity;
        QPushButton *saveButton, *abandonButton, *cancelButton;
        QCheckBox *warnCheckBox;
        QLabel *warnText;
        bool contextRequired_ = false;
        bool mayProceed_ = false;
};

class SaveOnExitDialogWidget : public QDialog
{
    Q_OBJECT
    G_OBJECT


    public:
        SaveOnExitDialogWidget(MainWindow *, Context *context, QList<RideItem*>);

    public slots:
        void saveClicked();
        void abandonClicked();
        void cancelClicked();
        void warnSettingClicked();

    protected:
        virtual bool saveRide(RideItem *rideItem);
        virtual void reportSaveError(const QString &error);
        virtual QList<RideItem *> currentDirtyActivities() const;

    private:
        struct DirtyActivity
        {
            QPointer<RideItem> item;
            ActivitySaveWorkflow::Identity identity;
            bool completed = false;
        };

        void appendDirtyActivity(RideItem *rideItem);
        bool reconcileDirtyActivities(bool &blocksAcceptance);
        bool activityIsCurrent(int row) const;

        QPointer<Context> context;
        QPointer<Athlete> athlete;
        QPointer<RideCache> cache;
        QList<DirtyActivity> dirtyList;
        QPushButton *saveButton, *abandonButton, *cancelButton;
        QCheckBox *exitWarnCheckBox;
        QLabel *warnText;
        bool contextRequired_ = false;

        QTableWidget *dirtyFiles;
};


struct GuardedOperationPreflightItem
{
    QPointer<RideItem> item;
    ActivitySaveWorkflow::Identity identity;

    GuardedOperationPreflightItem() = default;
    explicit GuardedOperationPreflightItem(RideItem *item)
    : item(item),
      identity(item
          ? ActivitySaveWorkflow::Identity{
                item->fileName, item->path, item->planned}
          : ActivitySaveWorkflow::Identity{})
    {
    }

    RideItem *data() const
    {
        return item.data();
    }

    bool matches() const
    {
        return item
            && item->fileName == identity.fileName
            && item->path == identity.path
            && item->planned == identity.planned;
    }
};

using GuardedOperationPreflightItems =
    QList<GuardedOperationPreflightItem>;
using OperationPreflightRelink =
    std::function<bool(
        RideItem *, GuardedOperationPreflightItems &,
        QString &)>;
using OperationPreflightSave =
    std::function<bool(
        const QList<RideItem*> &, QString &)>;
using OperationPreflightValidate =
    std::function<bool(QString &)>;
using OperationPreflightReload =
    std::function<RideFile *(RideItem *)>;

struct ProceedDialogSavedActivity
{
    QPointer<RideItem> item;
    ActivitySaveWorkflow::Identity identity;
};

struct ProceedDialogResult
{
    QList<ProceedDialogSavedActivity> savedActivities;

    bool savedIdentityFor(
        RideItem *item,
        ActivitySaveWorkflow::Identity &identity) const
    {
        bool found = false;
        for (const ProceedDialogSavedActivity &saved :
             savedActivities) {
            if (saved.item.data() != item) continue;
            if (found) return false;
            identity = saved.identity;
            found = true;
        }
        return found;
    }
};

extern GuardedOperationPreflightItems
guardOperationPreflightItems(
    const QList<RideItem*> &items);
extern bool resolveOperationPreflightItems(
    const GuardedOperationPreflightItems &guardedItems,
    QList<RideItem*> &items,
    QString &error);

extern bool saveOperationPreflightActivities(
    const GuardedOperationPreflightItems &dirtyItems,
    const OperationPreflightRelink &relink,
    const OperationPreflightSave &save,
    QString &error,
    const OperationPreflightValidate &validate =
        OperationPreflightValidate());
extern bool reloadOperationPreflightActivities(
    const GuardedOperationPreflightItems &items,
    const OperationPreflightReload &reload,
    QString &error,
    const OperationPreflightValidate &validate =
        OperationPreflightValidate());
extern RideFile *reloadDiscardedActivity(RideItem *item);
extern bool proceedDialog(
    Context *context,
    const RideCache::OperationPreCheck &check,
    ProceedDialogResult *result = nullptr);
extern bool relinkRideItems(
    Context *context,
    RideItem *rideItem,
    GuardedOperationPreflightItems &activities,
    QString &error);


#endif // _GC_SaveDialogs_h
