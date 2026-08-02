/*
 * Copyright (c) 2025 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
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

#ifndef _GC_PlanWizards_h
#define _GC_PlanWizards_h 1

#include "GoldenCheetah.h"

#include <QtGui>
#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QPointer>
#include <QSharedPointer>
#include <QTreeWidget>
#include <QCheckBox>
#include <QRadioButton>
#include <QStyledItemDelegate>
#include <QStackedWidget>

#include <functional>

#include "PlanBundle.h"
#include "Season.h"
#include "StyledItemDelegates.h"


class DisplayBox : public QFrame {
    Q_OBJECT

public:
    enum class Size {
        Large,
        Medium,
        Small,
        Tiny
    };

    explicit DisplayBox(const QString &title, const Size &size = Size::Large, bool highlight = false, QWidget *parent = nullptr);

    void setText(const QString &value);

private:
    QLabel *valueLabel;
    QLabel *titleLabel;
};


class TargetRangeBar : public QFrame
{
    Q_OBJECT
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor)

public:
    explicit TargetRangeBar(QString errorMsg, QWidget *parent = nullptr);

    void setResult(const QDate &start, const QDate &end, int activityCount, int deletedCount);
    void setFlashEnabled(bool enabled);
    void setCopyMsg(QString msg);

private:
    enum class State {
        Neutral,
        Warning,
        Error
    };

    QLabel *iconLabel;
    QLabel *textLabel;
    QColor baseColor;
    QColor borderColor;
    QColor hlColor;
    State currentState;
    const QString errorMsg;
    QString copyMsg;
    bool flashEnabled = true;

    void applyStateStyle(State state);
    QString formatDuration(const QDate &start, const QDate &end) const;
    QColor highlightColor() const;
    void setHighlightColor(const QColor& color);
    void flash();
};


class IndicatorDelegate : public QStyledItemDelegate
{
    public:
        enum Roles {
            IndicatorTypeRole = Qt::UserRole + 1, // [IndicatorType] Whether this item has an indicator
            IndicatorStateRole                    // [bool] Whether this items indicator is checked
        };

        enum IndicatorType {
            NoIndicator = 0,
            RadioIndicator = 1,
            CheckIndicator = 2
        };

        explicit IndicatorDelegate(QObject *parent = nullptr);

        void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
        bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;
        QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};


struct GuardedRideItem {
    QPointer<RideItem> rideItem;
    QString fileName;
    QString path;
    bool planned = false;

    GuardedRideItem() = default;
    explicit GuardedRideItem(RideItem *rideItem);

    bool matches(RideItem *candidate) const;
};


struct SourceRide {
    GuardedRideItem source;
    QDate sourceDate;
    QDate targetDate;
    bool selected = false;
    int conflictGroup = -1;
    bool targetBlocked = false;

    SourceRide() = default;
    SourceRide(RideItem *rideItem, const QDate &sourceDate,
               const QDate &targetDate, bool selected,
               int conflictGroup, bool targetBlocked);
};


class RepeatPlanWorkflowGuard
{
    public:
        RepeatPlanWorkflowGuard(
            QObject *wizard, QObject *context,
            QObject *athlete, QObject *cache,
            QObject *tab)
        : wizard(wizard), context(context), athlete(athlete),
          cache(cache), tab(tab)
        {
        }

        bool allAlive() const
        {
            return wizard && context && athlete && cache && tab;
        }

        bool canAccessPage() const
        {
            return allAlive();
        }

    private:
        QPointer<QObject> wizard;
        QPointer<QObject> context;
        QPointer<QObject> athlete;
        QPointer<QObject> cache;
        QPointer<QObject> tab;
};


class RepeatPlanWorkflowState
{
    public:
        void markInputsValid()
        {
            markInputsValid(true);
        }

        bool markInputsValid(bool hasCopyInputs)
        {
            inputsValid = hasCopyInputs;
            replacementComplete = false;
            return inputsValid;
        }

        bool canReplacePlan() const
        {
            return inputsValid;
        }

        void markReplacementComplete()
        {
            replacementComplete = inputsValid;
        }

        bool canAccept(
            const RepeatPlanWorkflowGuard &guard) const
        {
            return inputsValid && replacementComplete
                && guard.allAlive();
        }

    private:
        bool inputsValid = false;
        bool replacementComplete = false;
};


enum class RepeatPlanReplacementDisposition
{
    OwnerLost,
    Failed,
    Complete
};

Q_DECLARE_METATYPE(RepeatPlanReplacementDisposition)


inline bool repeatPlanReplacementInputsAreUsable(
    const QList<RideItem*> &deletionItems,
    const QList<std::pair<RideItem*, QDate>>
        &sourceItemsAndTargets)
{
    if (sourceItemsAndTargets.isEmpty()) return false;
    for (RideItem *item : deletionItems) {
        if (!item) return false;
    }
    for (const auto &entry : sourceItemsAndTargets) {
        if (!entry.first || !entry.second.isValid()) return false;
    }
    return true;
}


inline RepeatPlanReplacementDisposition
repeatPlanReplacementDisposition(
    bool ownersAlive, bool cleanlyCompleted,
    int removedCount, int expectedRemovedCount,
    int addedCount, int expectedAddedCount)
{
    if (!ownersAlive)
        return RepeatPlanReplacementDisposition::OwnerLost;
    if (!cleanlyCompleted
        || removedCount != expectedRemovedCount
        || addedCount != expectedAddedCount) {
        return RepeatPlanReplacementDisposition::Failed;
    }
    return RepeatPlanReplacementDisposition::Complete;
}


class RepeatPlanWorkflowExecutionGuard
{
    public:
        RepeatPlanWorkflowExecutionGuard(
            QObject *owner, bool &inProgress)
        : owner(owner), inProgress(&inProgress),
          ownsExecution(owner && !inProgress)
        {
            if (ownsExecution)
                inProgress = true;
        }

        ~RepeatPlanWorkflowExecutionGuard()
        {
            if (ownsExecution && owner)
                *inProgress = false;
        }

        bool acquired() const
        {
            return ownsExecution;
        }

        RepeatPlanWorkflowExecutionGuard(
            const RepeatPlanWorkflowExecutionGuard &) = delete;
        RepeatPlanWorkflowExecutionGuard &operator=(
            const RepeatPlanWorkflowExecutionGuard &) = delete;
        RepeatPlanWorkflowExecutionGuard(
            RepeatPlanWorkflowExecutionGuard &&) = delete;
        RepeatPlanWorkflowExecutionGuard &operator=(
            RepeatPlanWorkflowExecutionGuard &&) = delete;

    private:
        QPointer<QObject> owner;
        bool *inProgress;
        bool ownsExecution;
};


using PlanExportOperation = std::function<bool()>;
using PlanExportFailureHandler = std::function<void()>;

inline bool runPlanExportCompletion(
    const PlanExportOperation &exportOperation,
    const PlanExportFailureHandler &failureHandler)
{
    if (exportOperation && exportOperation())
        return true;
    if (failureHandler) failureHandler();
    return false;
}


////////////////////////////////////////////////////////////////////////////////
// Repeat Wizard

class RepeatPlanWizard : public QWizard
{
    Q_OBJECT

    public:
        enum {
            Finalize = -1,
            PageSetup,
            PageActivities,
            PageSummary
        };

        RepeatPlanWizard(Context *context, const QDate &when, QWidget *parent = nullptr);

        QList<SourceRide> sourceRides;

        QDate getTargetRangeStart() const;
        QDate getTargetRangeEnd() const;
        int getPlannedInTargetRange() const;
        const QList<GuardedRideItem> &getDeletionList() const;

        void updateTargetRange();
        void updateTargetRange(QDate sourceStart, QDate sourceEnd, bool keepGap, bool preferOriginal);
        QPointer<RideCache> resolvePageRideCache();

    signals:
        void targetRangeChanged();

    protected:
        virtual void done(int result) override;

    private:
        bool resolveOwners(
            Context *&resolvedContext,
            Athlete *&resolvedAthlete,
            RideCache *&resolvedCache,
            AthleteTab *&resolvedTab) const;

        QPointer<Context> context;
        QPointer<QObject> athleteLifetime;
        QPointer<QObject> cacheLifetime;
        QPointer<QObject> tabLifetime;
        QDate sourceRangeStart;
        QDate sourceRangeEnd;
        QDate targetRangeStart;
        QDate targetRangeEnd;
        int frontGap = 0;
        QList<GuardedRideItem> deletionList;
        bool keepGap = false;
        bool preferOriginal = false;
        bool completionInProgress = false;
};


class RepeatPlanPageSetup : public QWizardPage
{
    Q_OBJECT

    public:
        RepeatPlanPageSetup(Context *context, const QDate &when, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool isComplete() const override;

    private:
        QDateEdit *startDate;
        QDateEdit *endDate;
        QCheckBox *keepGapCheck;
        QRadioButton *originalRadio;
        QRadioButton *currentRadio;
        TargetRangeBar *targetRangeBar;

    private slots:
        void refresh();
};


class RepeatPlanPageActivities : public QWizardPage
{
    Q_OBJECT

    public:
        RepeatPlanPageActivities(Context *context, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool isComplete() const override;
        void cleanupPage() override;

    private:
        QTreeWidget *activityTree;
        TargetRangeBar *targetRangeBar;
        int numSelected = 0;
        QMetaObject::Connection dataChangedConnection;
};


class RepeatPlanPageSummary : public QWizardPage
{
    Q_OBJECT

    public:
        RepeatPlanPageSummary(Context *context, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;

    private:
        QLabel *planLabel;
        QTreeWidget *planTree;
        QLabel *deletionLabel;
        QTreeWidget *deletionTree;
        TargetRangeBar *targetRangeBar;
};


////////////////////////////////////////////////////////////////////////////////
// Export Wizard

class ExportPlanWizard : public QWizard
{
    Q_OBJECT

    public:
        enum {
            Finalize = -1,
            PageSetup,
            PageActivities,
            PageMetadata,
            PageSummary
        };

        ExportPlanWizard(Context *context, Season const * const preselectSeason = nullptr, QWidget *parent = nullptr);

        QList<SourceRide> sourceRides;

        PlanExportDescription &description();
        int getPlannedInTargetRange() const;

        void updateRange();
        void updateRange(QDate sourceStart, QDate sourceEnd, bool preferOriginal, bool force = false);

        QString expandedDescription() const;

    signals:
        void rangeChanged();

    protected:
        virtual void done(int result) override;

    private:
        Context *context;
        PlanExportDescription _description;
};


class ExportPlanPageSetup : public QWizardPage
{
    Q_OBJECT

    public:
        ExportPlanPageSetup(Context *context, Season const * const preselectSeason = nullptr, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool isComplete() const override;

    private:
        Context *context;
        QDateEdit *startDate;
        QDateEdit *endDate;
        QRadioButton *originalRadio;
        QRadioButton *currentRadio;
        TargetRangeBar *targetRangeBar;

    private slots:
        void refresh();
};


class ExportPlanPageActivities : public QWizardPage
{
    Q_OBJECT

    public:
        ExportPlanPageActivities(Context *context, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool isComplete() const override;
        void cleanupPage() override;

    private:
        Context *context;
        QTreeWidget *activityTree;
        TargetRangeBar *targetRangeBar;
        int numSelected = 0;
        QMetaObject::Connection dataChangedConnection;
};


class ExportPlanPageMetadata : public QWizardPage
{
    Q_OBJECT

    public:
        ExportPlanPageMetadata(Context *context, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool validatePage() override;
        bool isComplete() const override;
        void cleanupPage() override;

    private:
        Context *context;
        QLineEdit *authorEdit;
        QLineEdit *nameEdit;
        QLineEdit *copyrightEdit;
        QTextEdit *descriptionEdit;
        TargetRangeBar *targetRangeBar;
};


class ExportPlanPageSummary : public QWizardPage
{
    Q_OBJECT

    public:
        ExportPlanPageSummary(Context *context, QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool isComplete() const override;

    private:
        Context *context;

        DisplayBox *nameBox;
        DisplayBox *authorBox;
        DisplayBox *sportBox;
        DisplayBox *durationBox;
        DisplayBox *countBox;
        DisplayBox *copyrightBox;
        QLabel *descriptionValue;
        QTreeWidget *planTree;
        DirectoryPathWidget *outputPathWidget;

        QString sanitizeFilename(QString input) const;
};


////////////////////////////////////////////////////////////////////////////////
// Import Wizard

template<typename Reader>
class PlanImportReaderLease
{
    public:
        PlanImportReaderLease(
            QObject *owner,
            const QSharedPointer<Reader> &reader)
        : owner(owner), retainedReader(reader)
        {
        }

        bool available() const
        {
            return owner && !retainedReader.isNull();
        }

        bool ownerAlive() const
        {
            return owner;
        }

        Reader *reader() const
        {
            return retainedReader.data();
        }

        const QSharedPointer<Reader> &sharedReader() const
        {
            return retainedReader;
        }

    private:
        QPointer<QObject> owner;
        QSharedPointer<Reader> retainedReader;
};


class ImportPlanWizard : public QWizard
{
    Q_OBJECT

    public:
        enum {
            Finalize = -1,
            PageSetup,
            PageActivities,
            PageSummary,
            PageResult
        };

        ImportPlanWizard(Context *context, QDate targetDay, QWidget *parent = nullptr);

        QSharedPointer<PlanBundleReader> planReader;
};


class ImportPlanPageSetup : public QWizardPage
{
    Q_OBJECT

    public:
        ImportPlanPageSetup(QWidget *parent = nullptr);

        int nextId() const override;
        bool isComplete() const override;

    private:
        QStackedWidget *overviewStack;
        QCheckBox *gapDayCheck;
        QLabel *overviewEmpty;
        QLabel *overviewError;
        QFrame *statusFrame;
        QLabel *statusLabel;
        DisplayBox *nameBox;
        DisplayBox *authorBox;
        DisplayBox *sportBox;
        DisplayBox *durationBox;
        DisplayBox *countBox;
        DisplayBox *copyrightBox;
        QLabel *descriptionValue;
        TargetRangeBar *targetRangeBar;

    private slots:
        void bundlePathChanged(QString path);
        void updateRanges();
};


class ImportPlanPageActivities : public QWizardPage
{
    Q_OBJECT

    public:
        ImportPlanPageActivities(QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool isComplete() const override;
        void cleanupPage() override;

    private:
        QTreeWidget *activityTree;
        TargetRangeBar *targetRangeBar;
        QMetaObject::Connection dataChangedConnection;
};


class ImportPlanPageSummary : public QWizardPage
{
    Q_OBJECT

    public:
        ImportPlanPageSummary(QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;
        bool validatePage() override;

    private:
        QLabel *planLabel;
        QTreeWidget *planTree;
        QLabel *deletionLabel;
        QTreeWidget *deletionTree;
        TargetRangeBar *targetRangeBar;
};


class ImportPlanPageResult : public QWizardPage
{
    Q_OBJECT

    public:
        ImportPlanPageResult(QWidget *parent = nullptr);

        int nextId() const override;
        void initializePage() override;

    private:
        QLabel *label;
};

#endif
