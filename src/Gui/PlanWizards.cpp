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

#include "PlanWizards.h"

#include <algorithm>
#include <QtGui>
#include <QScrollArea>
#include <QButtonGroup>

#include "Context.h"
#include "Athlete.h"
#include "AthleteTab.h"
#include "Seasons.h"
#include "Colors.h"
#include "SaveDialogs.h"

#define HLO "<span style='font-weight:600;'>"
#define HLC "</span>"

#define ICON_COLOR QColor("#F79130")
#ifdef Q_OS_MAC
#define ICON_SIZE 250
#define ICON_MARGIN 0
#define ICON_TYPE QWizard::BackgroundPixmap
#else
#define ICON_SIZE 125
#define ICON_MARGIN 5
#define ICON_TYPE QWizard::LogoPixmap
#endif

static constexpr int IndexRole = Qt::UserRole + 100;
static const QString planExtension(".gcplan");


namespace {

class OverrideCursorScope
{
    public:
        OverrideCursorScope()
        {
            QApplication::setOverrideCursor(
                QCursor(Qt::WaitCursor));
        }

        ~OverrideCursorScope()
        {
            QApplication::restoreOverrideCursor();
        }
};


class NoSwitchScope
{
    public:
        explicit NoSwitchScope(AthleteTab *tab)
        : tab(tab), previousValue(
              tab ? tab->noSwitch() : false)
        {
            if (this->tab)
                this->tab->setNoSwitch(true);
        }

        ~NoSwitchScope()
        {
            if (tab)
                tab->setNoSwitch(previousValue);
        }

    private:
        QPointer<AthleteTab> tab;
        bool previousValue;
};

}


GuardedRideItem::GuardedRideItem
(RideItem *rideItem)
: rideItem(rideItem),
  fileName(rideItem ? rideItem->fileName : QString()),
  path(rideItem ? rideItem->path : QString()),
  planned(rideItem && rideItem->planned)
{
}


bool
GuardedRideItem::matches
(RideItem *candidate) const
{
    return candidate
        && rideItem.data() == candidate
        && candidate->fileName == fileName
        && candidate->path == path
        && candidate->planned == planned;
}


SourceRide::SourceRide
(RideItem *rideItem, const QDate &sourceDate,
 const QDate &targetDate, bool selected,
 int conflictGroup, bool targetBlocked)
: source(rideItem), sourceDate(sourceDate),
  targetDate(targetDate), selected(selected),
  conflictGroup(conflictGroup),
  targetBlocked(targetBlocked)
{
}


static RideItem *
resolveGuardedRide
(const GuardedRideItem &guarded, RideCache *cache)
{
    RideItem *const item = guarded.rideItem.data();
    if (!cache || !guarded.matches(item))
        return nullptr;
    return cache->getRide(
        guarded.fileName, guarded.planned) == item
        ? item : nullptr;
}


static bool
resolveGuardedRides
(const QList<GuardedRideItem> &guardedItems,
 RideCache *cache, QList<RideItem*> &items)
{
    items.clear();
    items.reserve(guardedItems.size());
    for (const GuardedRideItem &guarded : guardedItems) {
        RideItem *const item = resolveGuardedRide(
            guarded, cache);
        if (!item) {
            items.clear();
            return false;
        }
        items.append(item);
    }
    return true;
}


static bool
resolveRepeatPlanSources
(const QList<SourceRide> &sourceRides,
 RideCache *cache,
 QList<std::pair<RideItem*, QDate>> &planList)
{
    planList.clear();
    for (const SourceRide &sourceRide : sourceRides) {
        if (!sourceRide.selected
            || sourceRide.targetBlocked) {
            continue;
        }
        RideItem *const item = resolveGuardedRide(
            sourceRide.source, cache);
        if (!item) {
            planList.clear();
            return false;
        }
        planList.append(
            std::pair<RideItem*, QDate> {
                item, sourceRide.targetDate });
    }
    return true;
}


////////////////////////////////////////////////////////////////////////////////
// DisplayBox::DisplayBox

DisplayBox::DisplayBox
(const QString &title, const Size &size, bool highlight, QWidget *parent)
: QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);

    QPalette pal = palette();
    if (highlight) {
        QColor palHighlightColor = pal.color(QPalette::Active, QPalette::Highlight);
        palHighlightColor.setAlpha(30);
        QColor highlightColor = GCColor::blendedColor(palHighlightColor, pal.color(QPalette::Base));
        pal.setColor(QPalette::Window, highlightColor);
        pal.setColor(QPalette::Text, GCColor::invertColor(highlightColor));
    } else {
        pal.setColor(QPalette::Window, pal.color(QPalette::Base));
    }
    setAutoFillBackground(true);
    setPalette(pal);

    valueLabel = new QLabel();
    valueLabel->setAlignment(Qt::AlignLeft);
    valueLabel->setWordWrap(true);
    QFont valueFont = valueLabel->font();
    if (size == Size::Large) {
        valueFont.setPointSizeF(valueFont.pointSizeF() * 1.3);
    } else if (size == Size::Medium) {
        valueFont.setPointSizeF(valueFont.pointSizeF() * 1.15);
    } else if (size == Size::Small) {
        valueFont.setPointSizeF(valueFont.pointSizeF() * 1.0);
    } else if (size == Size::Tiny) {
        valueFont.setPointSizeF(valueFont.pointSizeF() * 0.85);
    }
    valueFont.setWeight(QFont::Medium);
    valueLabel->setFont(valueFont);

    titleLabel = new QLabel(title);
    titleLabel->setAlignment(Qt::AlignLeft);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.7);
    titleFont.setWeight(QFont::Light);
    titleLabel->setFont(titleFont);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12 * dpiXFactor, 8 * dpiYFactor, 12 * dpiXFactor, 8 * dpiYFactor);
    layout->addWidget(titleLabel, 0);
    layout->addWidget(valueLabel, 1);
}


void
DisplayBox::setText
(const QString &value)
{
    if (! value.trimmed().isEmpty()) {
        valueLabel->setText(value.trimmed());
    } else {
        valueLabel->setText(tr("N/A"));
    }
}


////////////////////////////////////////////////////////////////////////////////
// TargetRangeBar

TargetRangeBar::TargetRangeBar
(QString errorMsg, QWidget *parent)
: QFrame(parent), currentState(State::Neutral), errorMsg(errorMsg)
{
    setObjectName("TargetRangeBar");

    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Plain);
    setLineWidth(1 * dpiXFactor);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8 * dpiXFactor, 4 * dpiYFactor, 8 * dpiXFactor, 4 * dpiYFactor);
    layout->setSpacing(6 * dpiXFactor);

    iconLabel = new QLabel(this);
    iconLabel->setFixedWidth(16 * dpiXFactor);
    iconLabel->setAlignment(Qt::AlignCenter);

    textLabel = new QLabel();
    textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    textLabel->setWordWrap(false);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(iconLabel);
    layout->addWidget(textLabel);

    applyStateStyle(State::Neutral);
}


void
TargetRangeBar::setResult
(const QDate &start, const QDate &end, int activityCount, int deletedCount)
{
    QString text;
    if (activityCount == 0) {
        currentState = State::Error;
        text = errorMsg;
    } else {
        QLocale locale;
        QString localFormat = locale.dateFormat(QLocale::ShortFormat);
        QString customFormat = "ddd, " + localFormat;

        currentState = State::Neutral;
        QString duration = formatDuration(start, end);
        text = QString("%1 - %2 (%3) • ")
                      .arg(locale.toString(start, customFormat))
                      .arg(locale.toString(end, customFormat))
                      .arg(duration);
        if (! copyMsg.isEmpty()) {
            text += copyMsg.arg(activityCount);
        } else {
            text += tr("%1 to copy").arg(activityCount);
        }

        if (deletedCount > 0) {
            text += " • " + tr("%1 to remove").arg(deletedCount);
            currentState = State::Warning;
        }
    }
    if (text != textLabel->text()) {
        textLabel->setText(text);
        applyStateStyle(currentState);
        if (flashEnabled) {
            flash();
        }
    }
}


void
TargetRangeBar::setFlashEnabled
(bool enabled)
{
    flashEnabled = enabled;
}


void
TargetRangeBar::setCopyMsg
(QString msg)
{
    copyMsg = msg;
}


QString
TargetRangeBar::formatDuration
(const QDate &start, const QDate &end) const
{
    if (! start.isValid() || ! end.isValid()) {
        return "";
    }
    int days = std::abs(start.daysTo(end)) + 1;
    if (days < 14) {
        return tr("%1 d").arg(days);
    } else {
        return tr("%1 w").arg((days + 6) / 7);
    }
}


void
TargetRangeBar::applyStateStyle
(State state)
{
    QIcon icon;
    QColor accentColor;
    switch (state) {
    case State::Neutral:
        icon = style()->standardIcon(QStyle::SP_MessageBoxInformation);
        accentColor = Qt::gray;
        break;
    case State::Warning:
        icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        accentColor = QColor(255, 193, 7);
        break;
    case State::Error:
        icon = style()->standardIcon(QStyle::SP_MessageBoxCritical);
        accentColor = Qt::red;
        break;
    }
    QColor bg = QApplication::palette().window().color();
    accentColor.setAlpha(20);
    baseColor = GCColor::blendedColor(accentColor, bg);
    accentColor.setAlpha(200);
    borderColor = GCColor::blendedColor(accentColor, bg);
    iconLabel->setPixmap(icon.pixmap(14 * dpiXFactor, 14 * dpiYFactor));
    setStyleSheet(QString("#TargetRangeBar { background-color: %1; border: 1px solid %2; }")
                         .arg(baseColor.name())
                         .arg(borderColor.name()));
}


QColor
TargetRangeBar::highlightColor
() const
{
    return hlColor;
}


void
TargetRangeBar::setHighlightColor
(const QColor &color)
{
    hlColor = color;
    QColor blended;
    blended.setRed((baseColor.red() + color.red()) / 2);
    blended.setGreen((baseColor.green() + color.green()) / 2);
    blended.setBlue((baseColor.blue() + color.blue()) / 2);
    setStyleSheet(QString("#TargetRangeBar { background-color: %1; border: 1px solid %2; }")
                         .arg(blended.name())
                         .arg(borderColor.name()));
}


void
TargetRangeBar::flash
()
{
    QPropertyAnimation *anim = new QPropertyAnimation(this, "highlightColor");
    anim->setDuration(350);
    anim->setStartValue(GColor(CCALPLANNED));
    anim->setEndValue(baseColor);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}


////////////////////////////////////////////////////////////////////////////////
// IndicatorDelegate

IndicatorDelegate::IndicatorDelegate
(QObject *parent)
: QStyledItemDelegate(parent)
{
}


void
IndicatorDelegate::paint
(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (index.column() != 0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    const bool isRadio = index.data(IndicatorTypeRole).toInt() == RadioIndicator;
    const bool isCheck = index.data(IndicatorTypeRole).toInt() == CheckIndicator;
    if (! isRadio && ! isCheck) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    const int size = style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, opt.widget);
    const int spacing = 4 * dpiXFactor;
    QRect indicatorRect(opt.rect.left() + spacing, opt.rect.top() + (opt.rect.height() - size) / 2, size, size);

    QStyleOptionButton buttonOpt;
    buttonOpt.rect = indicatorRect;
    buttonOpt.state = QStyle::State_Enabled;
    buttonOpt.state |= index.data(IndicatorStateRole).toBool() ? QStyle::State_On : QStyle::State_Off;
    if (opt.state & QStyle::State_MouseOver) {
        buttonOpt.state |= QStyle::State_MouseOver;
    }

    style->drawPrimitive(isRadio ? QStyle::PE_IndicatorRadioButton : QStyle::PE_IndicatorCheckBox, &buttonOpt, painter, opt.widget);

    QRect textRect = opt.rect;
    textRect.setLeft(indicatorRect.right() + spacing);
    painter->save();
    painter->setClipRect(textRect);
    QColor textColor = (opt.state & QStyle::State_Selected) ? opt.palette.highlightedText().color() : opt.palette.text().color();
    painter->setPen(textColor);
    painter->drawText(textRect, opt.displayAlignment, index.data(Qt::DisplayRole).toString());
    painter->restore();
}


bool
IndicatorDelegate::editorEvent
(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index)
{
    Qt::ItemFlags flags = model->flags(index);
    if (! (flags & Qt::ItemIsEnabled)) {
        return false;
    }
    const bool isRadio = index.data(IndicatorTypeRole).toInt() == RadioIndicator;
    const bool isCheck = index.data(IndicatorTypeRole).toInt() == CheckIndicator;
    if (! isRadio && ! isCheck) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    } else if (event->type() != QEvent::MouseButtonRelease) {
        return false;
    } else if (isRadio) {
        model->setData(index, true, IndicatorStateRole);
        QModelIndex parent = index.parent();
        for (int row = 0; row < model->rowCount(parent); ++row) {
            QModelIndex sibling = model->index(row, index.column(), parent);
            if (sibling != index && sibling.data(IndicatorTypeRole).toInt() == RadioIndicator) {
                model->setData(sibling, false, IndicatorStateRole);
            }
        }
        return true;
    } else if (isCheck) {
        model->setData(index, ! index.data(IndicatorStateRole).toBool(), IndicatorStateRole);
        return true;
    }
    return false;
}


QSize
IndicatorDelegate::sizeHint
(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const bool isRadio = index.data(IndicatorTypeRole).toInt() == RadioIndicator;
    const bool isCheck = index.data(IndicatorTypeRole).toInt() == CheckIndicator;
    if ((! isRadio && ! isCheck) || index.column() != 0) {
        return QStyledItemDelegate::sizeHint(option, index);
    }
    QStyle *style = option.widget ? option.widget->style() : QApplication::style();
    const int indicatorWidth = style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, option.widget);
    const int spacing = 4 * dpiXFactor;
    QFontMetrics fm(option.font);
    int textWidth = fm.horizontalAdvance(index.data(Qt::DisplayRole).toString());
    return QSize(indicatorWidth + spacing + textWidth + spacing, QStyledItemDelegate::sizeHint(option, index).height());
}


////////////////////////////////////////////////////////////////////////////////
// RepeatPlanWizard

RepeatPlanWizard::RepeatPlanWizard
(Context *context, const QDate &when, QWidget *parent)
: QWizard(parent), context(context),
  athleteLifetime(context ? context->athlete : nullptr),
  cacheLifetime(
      context && context->athlete
      ? context->athlete->rideCache : nullptr),
  tabLifetime(context ? context->tab : nullptr),
  targetRangeStart(when), targetRangeEnd(when)
{
    setWindowTitle(tr("Repeat Plan"));
    setMinimumSize(800 * dpiXFactor, 750 * dpiYFactor);
    setModal(true);

#ifdef Q_OS_MAC
    setWizardStyle(QWizard::MacStyle);
#else
    setWizardStyle(QWizard::ModernStyle);
#endif
    setPixmap(ICON_TYPE, svgAsColoredPixmap(":images/breeze/media-playlist-repeat.svg", QSize(ICON_SIZE * dpiXFactor, ICON_SIZE * dpiYFactor), ICON_MARGIN * dpiXFactor, ICON_COLOR));

    setPage(PageSetup, new RepeatPlanPageSetup(context, when));
    setPage(PageActivities, new RepeatPlanPageActivities(context));
    setPage(PageSummary, new RepeatPlanPageSummary(context));
    setStartId(PageSetup);
}


bool
RepeatPlanWizard::resolveOwners
(Context *&resolvedContext,
 Athlete *&resolvedAthlete,
 RideCache *&resolvedCache,
 AthleteTab *&resolvedTab) const
{
    resolvedContext = context.data();
    resolvedAthlete = static_cast<Athlete*>(
        athleteLifetime.data());
    resolvedCache = static_cast<RideCache*>(
        cacheLifetime.data());
    resolvedTab = static_cast<AthleteTab*>(
        tabLifetime.data());

    return resolvedContext && resolvedAthlete
        && resolvedCache && resolvedTab
        && resolvedContext->athlete == resolvedAthlete
        && resolvedContext->tab == resolvedTab
        && resolvedAthlete->context == resolvedContext
        && resolvedAthlete->rideCache == resolvedCache;
}


QPointer<RideCache>
RepeatPlanWizard::resolvePageRideCache
()
{
    Context *resolvedContext = nullptr;
    Athlete *resolvedAthlete = nullptr;
    RideCache *resolvedCache = nullptr;
    AthleteTab *resolvedTab = nullptr;
    if (!resolveOwners(
            resolvedContext, resolvedAthlete,
            resolvedCache, resolvedTab)) {
        return nullptr;
    }

    const RepeatPlanWorkflowGuard guard(
        this, resolvedContext, resolvedAthlete,
        resolvedCache, resolvedTab);
    return guard.canAccessPage()
        ? QPointer<RideCache>(resolvedCache)
        : QPointer<RideCache>();
}


QDate
RepeatPlanWizard::getTargetRangeStart
() const
{
    return targetRangeStart;
}


QDate
RepeatPlanWizard::getTargetRangeEnd
() const
{
    return targetRangeEnd;
}


const QList<GuardedRideItem>&
RepeatPlanWizard::getDeletionList
() const
{
    return deletionList;
}


void
RepeatPlanWizard::updateTargetRange
()
{
    updateTargetRange(sourceRangeStart, sourceRangeEnd, keepGap, preferOriginal);
}


void
RepeatPlanWizard::updateTargetRange
(QDate sourceStart, QDate sourceEnd, bool keepGap, bool preferOriginal)
{
    Context *resolvedContext = nullptr;
    Athlete *athlete = nullptr;
    RideCache *cache = nullptr;
    AthleteTab *tab = nullptr;
    if (!resolveOwners(
            resolvedContext, athlete, cache, tab)) {
        return;
    }
    Q_UNUSED(resolvedContext)
    Q_UNUSED(athlete)
    Q_UNUSED(tab)
    if (   sourceRangeStart != sourceStart
        || sourceRangeEnd != sourceEnd
        || keepGap != this->keepGap
        || preferOriginal != this->preferOriginal) {

        sourceRangeStart = sourceStart;
        sourceRangeEnd = sourceEnd;
        this->keepGap = keepGap;
        this->preferOriginal = preferOriginal;

        sourceRides.clear();
        for (RideItem *rideItem : cache->rides()) {
            if (   rideItem == nullptr
                || ! rideItem->planned) {
                continue;
            }
            QDate rideDate = PlanBundle::getRideDate(rideItem, preferOriginal);
            if (   rideDate < sourceStart
                || rideDate > sourceEnd) {
                continue;
            }
            sourceRides << SourceRide { rideItem, rideDate, QDate(), true, -1, false };
        }
        std::sort(sourceRides.begin(), sourceRides.end(),
            [](const SourceRide &a, const SourceRide &b) { return a.sourceDate < b.sourceDate; });

        // Assume all planned activities from the range will be copied
        int prelimFrontGap = 0;
        if (! keepGap && ! sourceRides.isEmpty()) {
            prelimFrontGap = sourceStart.daysTo(sourceRides.first().sourceDate);
        }
        for (SourceRide &sourceRide : sourceRides) {
            sourceRide.targetDate = targetRangeStart.addDays(
                sourceRangeStart.daysTo(sourceRide.sourceDate) - prelimFrontGap);
        }

        // QDateTime of any planned RideItem must be unique
        QHash<QDateTime, int> targetKeyCount;
        for (const SourceRide &sourceRide : sourceRides) {
            RideItem *const item = resolveGuardedRide(
                sourceRide.source, cache);
            if (!item)
                continue;
            QDateTime key(
                sourceRide.targetDate,
                item->dateTime.time());
            targetKeyCount[key]++;
        }
        QHash<QDateTime, int> keyToGroup;
        int nextGroup = 0;
        for (SourceRide &sourceRide : sourceRides) {
            RideItem *const item = resolveGuardedRide(
                sourceRide.source, cache);
            if (!item)
                continue;
            QDateTime key(
                sourceRide.targetDate,
                item->dateTime.time());
            if (targetKeyCount[key] > 1) {
                if (! keyToGroup.contains(key)) {
                    keyToGroup[key] = nextGroup++;
                }
                sourceRide.conflictGroup = keyToGroup[key];
            } else {
                sourceRide.conflictGroup = -1;
            }
        }

        QHash<int, bool> groupHasSelection;
        for (SourceRide &sourceRide : sourceRides) {
            if (sourceRide.conflictGroup < 0) {
                continue;
            }
            if (! groupHasSelection.value(sourceRide.conflictGroup, false)) {
                sourceRide.selected = true;
                groupHasSelection[sourceRide.conflictGroup] = true;
            } else {
                sourceRide.selected = false;
            }
        }
    }

    sourceRides.erase(
        std::remove_if(
            sourceRides.begin(), sourceRides.end(),
            [cache](const SourceRide &sourceRide) {
                return !resolveGuardedRide(
                    sourceRide.source, cache);
            }),
        sourceRides.end());

    // Calculate frontGap and rangeLength
    frontGap = 0;
    int rangeLength = 0;
    if (! sourceRides.isEmpty()) {
        QDate firstSelectedDate;
        QDate lastSelectedDate;
        for (const SourceRide &sourceRide : sourceRides) {
            if (! sourceRide.selected) {
                continue;
            }
            if (firstSelectedDate.isNull()) {
                firstSelectedDate = sourceRide.sourceDate;
                if (! keepGap) {
                    frontGap = sourceStart.daysTo(firstSelectedDate);
                }
            }
            lastSelectedDate = sourceRide.sourceDate;
        }

        for (SourceRide &sourceRide : sourceRides) {
            sourceRide.targetDate = targetRangeStart.addDays(
                sourceRangeStart.daysTo(sourceRide.sourceDate) - frontGap);
        }

        if (firstSelectedDate.isValid()) {
            rangeLength = sourceStart.daysTo(sourceEnd);
            if (! keepGap) {
                rangeLength -= (frontGap + lastSelectedDate.daysTo(sourceEnd));
            }
        }
    }
    targetRangeEnd = targetRangeStart.addDays(rangeLength);

    // Find conflicting planned and linked activities (they wont be autodeleted)
    deletionList.clear();
    QSet<QDateTime> blockedKeys;
    for (RideItem *rideItem : cache->rides()) {
        if (   rideItem == nullptr
            || ! rideItem->planned) {
            continue;
        }
        QDate rideDate = rideItem->dateTime.date();
        if (   rideDate < targetRangeStart
            || rideDate > targetRangeEnd) {
            continue;
        }
        if (rideItem->hasLinkedActivity()) {
            blockedKeys.insert(QDateTime(rideDate, rideItem->dateTime.time()));
        } else {
            deletionList << GuardedRideItem(rideItem);
        }
    }
    for (SourceRide &sourceRide : sourceRides) {
        RideItem *const item = resolveGuardedRide(
            sourceRide.source, cache);
        if (!item)
            continue;
        QDateTime key(
            sourceRide.targetDate,
            item->dateTime.time());
        sourceRide.targetBlocked = blockedKeys.contains(key);
    }

    emit targetRangeChanged();
}


void
RepeatPlanWizard::done
(int result)
{
    if (completionInProgress)
        return;
    if (result != QDialog::Accepted) {
        QWizard::done(result);
        return;
    }

    RepeatPlanWorkflowExecutionGuard executionGuard(
        this, completionInProgress);
    if (!executionGuard.acquired())
        return;

    QPointer<RepeatPlanWizard> wizard(this);
    Context *guardedContext = nullptr;
    Athlete *guardedAthlete = nullptr;
    RideCache *guardedCache = nullptr;
    AthleteTab *guardedTab = nullptr;
    if (!resolveOwners(
            guardedContext, guardedAthlete,
            guardedCache, guardedTab)) {
        QMessageBox::warning(
            this, tr("Repeat Plan"),
            tr("The athlete was closed while the wizard was open."));
        return;
    }

    const RepeatPlanWorkflowGuard lifetimeGuard(
        wizard.data(), guardedContext, guardedAthlete,
        guardedCache, guardedTab);
    RepeatPlanWorkflowState workflowState;

    const auto ownersValid = [&]() {
        if (!wizard || !lifetimeGuard.allAlive())
            return false;

        Context *currentContext = nullptr;
        Athlete *currentAthlete = nullptr;
        RideCache *currentCache = nullptr;
        AthleteTab *currentTab = nullptr;
        return wizard->resolveOwners(
                currentContext, currentAthlete,
                currentCache, currentTab)
            && currentContext == guardedContext
            && currentAthlete == guardedAthlete
            && currentCache == guardedCache
            && currentTab == guardedTab;
    };
    const auto showWarning = [&](const QString &message) {
        if (!ownersValid())
            return;
        QMessageBox::warning(
            wizard.data(), tr("Repeat Plan"), message);
    };
    const auto resolveInputs = [&]
        (QList<RideItem*> &deletionItems,
         QList<std::pair<RideItem*, QDate>> &planList) {
        if (!ownersValid())
            return false;
        if (!resolveGuardedRides(
                wizard->deletionList, guardedCache,
                deletionItems)
            || !resolveRepeatPlanSources(
                wizard->sourceRides, guardedCache,
                planList)) {
            deletionItems.clear();
            planList.clear();
            return false;
        }
        if (!repeatPlanReplacementInputsAreUsable(
                deletionItems, planList)) {
            deletionItems.clear();
            planList.clear();
            return false;
        }
        return true;
    };

    QList<RideItem*> deletionItems;
    QList<std::pair<RideItem*, QDate>> planList;
    if (!resolveInputs(deletionItems, planList)) {
        showWarning(
            tr("The plan changed while the wizard was open. Review the activities and try again."));
        return;
    }
    if (!workflowState.markInputsValid(
            !planList.isEmpty())) {
        deletionItems.clear();
        planList.clear();
        showWarning(
            tr("No activities are available to copy. No activities were removed."));
        return;
    }
    deletionItems.clear();
    planList.clear();

    OverrideCursorScope cursorScope;
    NoSwitchScope noSwitchScope(guardedTab);

    if (!resolveInputs(deletionItems, planList)) {
        deletionItems.clear();
        planList.clear();
        showWarning(
            tr("The plan changed while preparing the update. No activities were removed."));
        return;
    }
    if (!workflowState.markInputsValid(
            !planList.isEmpty())) {
        deletionItems.clear();
        planList.clear();
        showWarning(
            tr("No activities are available to copy. No activities were removed."));
        return;
    }
    if (!workflowState.canReplacePlan())
        return;

    QSet<RideItem*> uniqueDeletionItems;
    for (RideItem *item : std::as_const(deletionItems))
        uniqueDeletionItems.insert(item);
    const int expectedRemovedCount = uniqueDeletionItems.size();
    const int expectedAddedCount = planList.size();
    const RideCache::PlannedReplacementResult replacement =
        guardedCache->replacePlannedActivities(
            deletionItems, planList);
    deletionItems.clear();
    planList.clear();
    const RepeatPlanReplacementDisposition disposition =
        repeatPlanReplacementDisposition(
            ownersValid(), replacement.committed,
            replacement.cleanlyCompleted(),
            replacement.removedCount, expectedRemovedCount,
            replacement.addedCount, expectedAddedCount);
    if (disposition
        == RepeatPlanReplacementDisposition::OwnerLost) {
        return;
    }
    if (disposition
        == RepeatPlanReplacementDisposition::Failed) {
        const QString fallback =
            tr("The existing plan was not changed.");
        showWarning(
            replacement.error.isEmpty()
            ? fallback
            : replacement.error);
        return;
    }
    workflowState.markReplacementComplete();

    QStringList notices;
    if (disposition
        == RepeatPlanReplacementDisposition::CommittedOwnerLost) {
        notices.append(replacement.error.isEmpty()
            ? tr("The plan files were replaced, but the athlete was closed before the activity view could be updated.")
            : replacement.error);
    } else if (disposition
        == RepeatPlanReplacementDisposition::CommittedWithIssues) {
        notices.append(replacement.error.isEmpty()
            ? tr("The plan files were replaced, but the activity list could not be updated completely.")
            : replacement.error);
    }
    if (replacement.removedCount != expectedRemovedCount) {
        notices.append(tr(
            "Expected to replace %1 existing activities, but the cache reported %2.")
            .arg(expectedRemovedCount)
            .arg(replacement.removedCount));
    }
    if (replacement.addedCount != expectedAddedCount) {
        notices.append(tr(
            "Expected to add %1 activities, but the cache reported %2.")
            .arg(expectedAddedCount)
            .arg(replacement.addedCount));
    }
    notices.append(replacement.warnings);
    notices.removeAll(QString());
    notices.removeDuplicates();
    if (!wizard)
        return;
    if (!notices.isEmpty())
        QMessageBox::warning(
            wizard.data(), tr("Repeat Plan"),
            notices.join(QLatin1Char('\n')));

    if (!wizard)
        return;
    wizard->QWizard::done(QDialog::Accepted);
}


////////////////////////////////////////////////////////////////////////////////
// RepeatPlanPageSetup

RepeatPlanPageSetup::RepeatPlanPageSetup
(Context *context, const QDate &when, QWidget *parent)
: QWizardPage(parent)
{
    QLocale locale;
    QString localFormat = locale.dateFormat(QLocale::ShortFormat);
    QString customFormat = "ddd, " + localFormat;

    setTitle(tr("Repeat Plan - Setup"));
    setSubTitle(tr("Define the time range and repetition strategy for copying activities. Optionally select a season or phase to prefill the dates; only those ending before the target date <b>%1</b> can be selected.")
                  .arg(locale.toString(when, customFormat)));

    QTreeWidget *seasonTree = new QTreeWidget();
    seasonTree->setColumnCount(1);
    basicTreeWidgetStyle(seasonTree, false);
    seasonTree->setHeaderHidden(true);
    seasonTree->resetIndentation();
    QTreeWidgetItem *currentSeason = nullptr;
    for (const Season &season : context->athlete->seasons->seasons) {
        QTreeWidgetItem *seasonItem = new QTreeWidgetItem();
        seasonItem->setData(0, Qt::DisplayRole, season.getName());
        seasonItem->setData(0, Qt::UserRole, season.getStart());
        seasonItem->setData(0, Qt::UserRole + 1, season.getEnd());
        seasonItem->setDisabled(DateRange(season.getStart(), season.getEnd()).pass(when) || season.getStart() > when);
        if (! seasonItem->isDisabled() && context->currentSeason() != nullptr && context->currentSeason()->id() == season.id()) {
            currentSeason = seasonItem;
        }
        for (const Phase &phase : season.phases) {
            QTreeWidgetItem *phaseItem = new QTreeWidgetItem();
            phaseItem->setData(0, Qt::DisplayRole, phase.getName());
            phaseItem->setData(0, Qt::UserRole, phase.getStart());
            phaseItem->setData(0, Qt::UserRole + 1, phase.getEnd());
            phaseItem->setDisabled(DateRange(phase.getStart(), phase.getEnd()).pass(when) || phase.getStart() > when);
            seasonItem->addChild(phaseItem);
            if (! phaseItem->isDisabled() && context->currentSeason() != nullptr && context->currentSeason()->id() == phase.id()) {
                currentSeason = phaseItem;
            }
        }
        seasonTree->addTopLevelItem(seasonItem);
    }

    startDate = new QDateEdit();
    startDate->setMaximumDate(when.addDays(-2));
    startDate->setCalendarPopup(true);
    startDate->setDisplayFormat(customFormat);

    endDate = new QDateEdit(when.addDays(-1));
    endDate->setMaximumDate(when.addDays(-1));
    endDate->setCalendarPopup(true);
    endDate->setDisplayFormat(customFormat);

    QHBoxLayout *dateRangeLayout = new QHBoxLayout();
    dateRangeLayout->addWidget(startDate);
    dateRangeLayout->addWidget(new QLabel(" - "));
    dateRangeLayout->addWidget(endDate);
    dateRangeLayout->addStretch();

    originalRadio = new QRadioButton(tr("As originally planned"));
    currentRadio = new QRadioButton(tr("As currently planned"));
    QButtonGroup *sourceDateGroup = new QButtonGroup(this);
    sourceDateGroup->setExclusive(true);
    sourceDateGroup->addButton(originalRadio);
    sourceDateGroup->addButton(currentRadio);
    originalRadio->setChecked(true);

    keepGapCheck = new QCheckBox(tr("Keep leading and trailing gaps"));
    keepGapCheck->setChecked(true);

    targetRangeBar = new TargetRangeBar(tr("No planned activities in source period"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);

    QVBoxLayout *form = new QVBoxLayout();
    form->addWidget(new QLabel(HLO + tr("Source Period") + HLC));
    form->addWidget(seasonTree);
    form->addLayout(dateRangeLayout);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(new QLabel(HLO + tr("Repetition Strategy") + HLC));
    form->addWidget(originalRadio);
    form->addWidget(currentRadio);
    form->addWidget(keepGapCheck);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(targetRangeBar);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    setLayout(all);

    connect(seasonTree, &QTreeWidget::currentItemChanged, this, [this, when](QTreeWidgetItem *current) {
        if (current != nullptr) {
            QDate seasonStart(current->data(0, Qt::UserRole).toDate());
            QDate seasonEnd(current->data(0, Qt::UserRole + 1).toDate());
            if (seasonEnd > when) {
                seasonEnd = when;
            }
            if (seasonStart > when) {
                seasonEnd = when;
            }

            QSignalBlocker b1(startDate);
            QSignalBlocker b2(endDate);
            startDate->setMaximumDate(seasonEnd);
            endDate->setMinimumDate(seasonStart);
            startDate->setDate(seasonStart);
            endDate->setDate(seasonEnd);

            refresh();
        }
    });
    connect(startDate, &QDateEdit::dateChanged, this, [this, seasonTree](QDate date) {
        endDate->setMinimumDate(date);
        seasonTree->setCurrentItem(nullptr);
        refresh();
    });
    connect(endDate, &QDateEdit::dateChanged, this, [this, seasonTree](QDate date) {
        startDate->setMaximumDate(date);
        seasonTree->setCurrentItem(nullptr);
        refresh();
    });
    connect(keepGapCheck, &QCheckBox::toggled, this, &RepeatPlanPageSetup::refresh);
    connect(sourceDateGroup, &QButtonGroup::idClicked, this, &RepeatPlanPageSetup::refresh);

    if (currentSeason != nullptr) {
        seasonTree->setCurrentItem(currentSeason);
        currentSeason->setSelected(true);
        if (currentSeason->parent() != nullptr) {
            currentSeason->parent()->setExpanded(true);
        }
    } else {
        startDate->setDate(QDate::currentDate().addDays(-7));
        endDate->setDate(QDate::currentDate().addDays(-1));
    }
}


int
RepeatPlanPageSetup::nextId
() const
{
    return RepeatPlanWizard::PageActivities;
}


void
RepeatPlanPageSetup::initializePage
()
{
    targetRangeBar->setFlashEnabled(false);
    refresh();
    targetRangeBar->setFlashEnabled(true);
}


bool
RepeatPlanPageSetup::isComplete
() const
{
    RepeatPlanWizard *rsw = qobject_cast<RepeatPlanWizard*>(wizard());
    return rsw && rsw->resolvePageRideCache()
        && rsw->sourceRides.count() > 0;
}


void
RepeatPlanPageSetup::refresh
()
{
    RepeatPlanWizard *rsw = qobject_cast<RepeatPlanWizard*>(wizard());
    if (rsw == nullptr) {
        return;
    }

    rsw->updateTargetRange(startDate->date(), endDate->date(), keepGapCheck->isChecked(), originalRadio->isChecked());

    if (!rsw->resolvePageRideCache()) {
        targetRangeBar->setResult(
            QDate(), QDate(), 0, 0);
        emit completeChanged();
        return;
    }

    targetRangeBar->setResult(rsw->getTargetRangeStart(), rsw->getTargetRangeEnd(), rsw->sourceRides.count(), rsw->getDeletionList().count());

    emit completeChanged();
}


////////////////////////////////////////////////////////////////////////////////
// RepeatPlanPageActivities

RepeatPlanPageActivities::RepeatPlanPageActivities
(Context *, QWidget *parent)
: QWizardPage(parent)
{
    setTitle(tr("Repeat Plan - Activities"));
    setSubTitle(tr("Review and choose the activities you wish to add to your plan."));

    setFinalPage(false);

    activityTree = new QTreeWidget();
    activityTree->setHeaderHidden(true);
    activityTree->setColumnCount(3);
    activityTree->setRootIsDecorated(false);
    activityTree->setSelectionMode(QAbstractItemView::SingleSelection);
    activityTree->setItemDelegate(new IndicatorDelegate());
    activityTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

    targetRangeBar = new TargetRangeBar(tr("No selected activities"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);

    QWidget *formWidget = new QWidget();
    QVBoxLayout *form = new QVBoxLayout(formWidget);
    form->addWidget(new QLabel(HLO + tr("Activities for your new plan") + HLC));
    form->addWidget(activityTree);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(targetRangeBar);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    setLayout(all);
}


int
RepeatPlanPageActivities::nextId
() const
{
    return RepeatPlanWizard::PageSummary;
}


void
RepeatPlanPageActivities::initializePage
()
{
    disconnect(dataChangedConnection);
    numSelected = 0;
    activityTree->clear();
    targetRangeBar->setFlashEnabled(false);
    targetRangeBar->setResult(QDate(), QDate(), 0, 0);

    RepeatPlanWizard *rsw = qobject_cast<RepeatPlanWizard*>(wizard());
    if (rsw == nullptr) {
        targetRangeBar->setFlashEnabled(true);
        emit completeChanged();
        return;
    }
    rsw->updateTargetRange();
    QPointer<RideCache> cache =
        rsw->resolvePageRideCache();
    if (!cache) {
        targetRangeBar->setFlashEnabled(true);
        emit completeChanged();
        return;
    }

    QLocale locale;
    int i = 0;
    while (i < rsw->sourceRides.count()) {
        const SourceRide &sourceRide = rsw->sourceRides[i];
        RideItem *const sourceItem =
            resolveGuardedRide(
                sourceRide.source,
                cache.data());
        if (!sourceItem) {
            ++i;
            continue;
        }

        if (sourceRide.conflictGroup < 0) {
            QTreeWidgetItem *item = new QTreeWidgetItem();
            item->setData(0, Qt::DisplayRole, locale.toString(sourceRide.sourceDate, QLocale::ShortFormat));
            item->setData(0, IndicatorDelegate::IndicatorTypeRole, IndicatorDelegate::CheckIndicator);
            item->setData(0, IndicatorDelegate::IndicatorStateRole, sourceRide.selected);
            item->setData(0, IndexRole, i);
            item->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(sourceItem));
            item->setData(2, Qt::DisplayRole, PlanBundle::getRideName(sourceItem));
            activityTree->addTopLevelItem(item);

            if (sourceRide.selected) {
                ++numSelected;
            }
            ++i;
        } else {
            int group = sourceRide.conflictGroup;
            QList<int> groupIndices;
            while (i < rsw->sourceRides.count() && rsw->sourceRides[i].conflictGroup == group) {
                groupIndices << i;
                ++i;
            }

            bool linkedConflict = std::any_of(groupIndices.begin(), groupIndices.end(), [rsw](int idx) { return rsw->sourceRides[idx].targetBlocked; });

            // Parent node — warning icon, shared target date, no checkbox
            QTreeWidgetItem *groupItem = new QTreeWidgetItem(activityTree);
            groupItem->setFirstColumnSpanned(true);
            groupItem->setIcon(0, style()->standardIcon(QStyle::SP_MessageBoxWarning));
            groupItem->setData(0, Qt::DisplayRole, tr("Choose one to plan"));
            if (linkedConflict) {
                groupItem->setData(0, Qt::DisplayRole, tr("Blocked by linked activity"));
            }
            groupItem->setExpanded(true);

            for (int idx : groupIndices) {
                const SourceRide &groupRide = rsw->sourceRides[idx];
                RideItem *const groupItemRide =
                    resolveGuardedRide(
                        groupRide.source,
                        cache.data());
                if (!groupItemRide)
                    continue;
                QTreeWidgetItem *child = new QTreeWidgetItem(groupItem);
                child->setData(0, Qt::DisplayRole, locale.toString(groupRide.sourceDate, QLocale::ShortFormat));
                child->setData(0, IndicatorDelegate::IndicatorTypeRole, IndicatorDelegate::RadioIndicator);
                child->setData(0, IndicatorDelegate::IndicatorStateRole, groupRide.selected);
                child->setData(0, IndexRole, idx);
                child->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(groupItemRide));
                child->setData(2, Qt::DisplayRole, PlanBundle::getRideName(groupItemRide));
            }
            ++numSelected;
        }
    }

    dataChangedConnection = connect(activityTree->model(), &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &index) {
        RepeatPlanWizard *rsw = qobject_cast<RepeatPlanWizard*>(wizard());
        if (rsw == nullptr) {
            return;
        }
        QModelIndex col0Index = index.siblingAtColumn(0);
        int indicatorType = col0Index.data(IndicatorDelegate::IndicatorTypeRole).toInt();
        if (indicatorType == IndicatorDelegate::NoIndicator) {
            return;
        }
        int i = col0Index.data(IndexRole).toInt();
        if (i < 0 || i >= rsw->sourceRides.size()) {
            return;
        }
        QPointer<RideCache> cache =
            rsw->resolvePageRideCache();
        if (!cache) {
            initializePage();
            return;
        }
        if (!resolveGuardedRide(
                rsw->sourceRides[i].source,
                cache.data())) {
            rsw->updateTargetRange();
            initializePage();
            return;
        }
        bool checked = col0Index.data(IndicatorDelegate::IndicatorStateRole).toBool();
        rsw->sourceRides[i].selected = checked;
        if (indicatorType == IndicatorDelegate::CheckIndicator) {
            numSelected += checked ? 1 : -1;
            emit completeChanged();
        }
        const int previousSourceCount =
            rsw->sourceRides.size();
        rsw->updateTargetRange();
        if (rsw->sourceRides.size()
            != previousSourceCount) {
            initializePage();
            return;
        }
        targetRangeBar->setResult(rsw->getTargetRangeStart(), rsw->getTargetRangeEnd(), numSelected, rsw->getDeletionList().count());
    });
    targetRangeBar->setResult(rsw->getTargetRangeStart(), rsw->getTargetRangeEnd(), numSelected, rsw->getDeletionList().count());
    targetRangeBar->setFlashEnabled(true);
}


bool
RepeatPlanPageActivities::isComplete
() const
{
    return numSelected > 0;
}


void
RepeatPlanPageActivities::cleanupPage
()
{
    disconnect(dataChangedConnection);
}


////////////////////////////////////////////////////////////////////////////////
// RepeatPlanPageSummary

RepeatPlanPageSummary::RepeatPlanPageSummary
(Context *, QWidget *parent)
: QWizardPage(parent)
{
    setTitle(tr("Repeat Plan - Summary"));
    setSubTitle(tr("Preview the plan updates, including planned additions and deletions. No changes will be made until you continue."));

    setFinalPage(true);

    planLabel = new QLabel(HLO + tr("New Plan Overview") + HLC);
    planTree = new QTreeWidget();
    planTree->setColumnCount(5);
    basicTreeWidgetStyle(planTree, false);
    planTree->setHeaderHidden(true);

    deletionLabel = new QLabel(HLO + tr("Planned Activities Marked for Deletion") + HLC);
    deletionTree = new QTreeWidget();
    deletionTree->setColumnCount(3);
    basicTreeWidgetStyle(deletionTree, false);
    deletionTree->setHeaderHidden(true);

    targetRangeBar = new TargetRangeBar(tr("No selected activities"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);
    targetRangeBar->setFlashEnabled(false);

    QWidget *formWidget = new QWidget();
    QVBoxLayout *form = new QVBoxLayout(formWidget);
    form->addWidget(planLabel);
    form->addWidget(planTree);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(deletionLabel);
    form->addWidget(deletionTree);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(targetRangeBar);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    setLayout(all);
}


int
RepeatPlanPageSummary::nextId
() const
{
    return RepeatPlanWizard::Finalize;
}


void
RepeatPlanPageSummary::initializePage
()
{
    planTree->clear();
    deletionTree->clear();

    planLabel->setVisible(false);
    planTree->setVisible(false);
    deletionLabel->setVisible(false);
    deletionTree->setVisible(false);
    targetRangeBar->setResult(QDate(), QDate(), 0, 0);

    RepeatPlanWizard *rsw = qobject_cast<RepeatPlanWizard*>(wizard());
    if (rsw == nullptr) {
        return;
    }
    rsw->updateTargetRange();
    QPointer<RideCache> cache =
        rsw->resolvePageRideCache();
    if (!cache)
        return;

    QLocale locale;
    int numSelected = 0;
    for (const SourceRide &sourceRide : rsw->sourceRides) {
        if (! sourceRide.selected) {
            continue;
        }
        RideItem *const sourceItem =
            resolveGuardedRide(
                sourceRide.source,
                cache.data());
        if (!sourceItem)
            continue;
        QTreeWidgetItem *planItem = new QTreeWidgetItem(planTree);
        planItem->setData(0, Qt::DisplayRole, locale.toString(sourceRide.sourceDate, QLocale::ShortFormat));
        planItem->setData(1, Qt::DisplayRole, "→");
        if (! sourceRide.targetBlocked) {
            planItem->setData(2, Qt::DisplayRole, locale.toString(sourceRide.targetDate, QLocale::ShortFormat));
            ++numSelected;
        } else {
            QFont font;
            font.setItalic(true);
            planItem->setData(0, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(1, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(2, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(2, Qt::FontRole, font);
            planItem->setData(2, Qt::DisplayRole, tr("skipped"));
            planItem->setData(3, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(4, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
        }
        planItem->setData(3, Qt::DisplayRole, PlanBundle::getRideSport(sourceItem));
        planItem->setData(4, Qt::DisplayRole, PlanBundle::getRideName(sourceItem));
    }
    planLabel->setVisible(true);
    planTree->setVisible(true);
    const QList<GuardedRideItem> &deletionList =
        rsw->getDeletionList();
    if (deletionList.count() > 0) {
        for (const GuardedRideItem &guarded :
             deletionList) {
            RideItem *const rideItem =
                resolveGuardedRide(
                    guarded,
                    cache.data());
            if (!rideItem)
                continue;
            QTreeWidgetItem *deletionItem = new QTreeWidgetItem(deletionTree);
            deletionItem->setData(0, Qt::DisplayRole, locale.toString(rideItem->dateTime.date(), QLocale::ShortFormat));
            deletionItem->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(rideItem));
            deletionItem->setData(2, Qt::DisplayRole, PlanBundle::getRideName(rideItem));
        }
        deletionLabel->setVisible(true);
        deletionTree->setVisible(true);
    }
    targetRangeBar->setResult(rsw->getTargetRangeStart(), rsw->getTargetRangeEnd(), numSelected, rsw->getDeletionList().count());
}


////////////////////////////////////////////////////////////////////////////////
// ExportPlanWizard

ExportPlanWizard::ExportPlanWizard
(Context *context, Season const * const preselectSeason, QWidget *parent)
: QWizard(parent), context(context)
{
    setWindowTitle(tr("Export Plan"));
    setMinimumSize(800 * dpiXFactor, 750 * dpiYFactor);
    setModal(true);

    _description.author = context->athlete->cyclist;
    _description.description = tr(R"(## $NAME by $AUTHOR

$SPORT

### Prerequisites
*Minimum fitness base, experience level, or equipment required.*

### Training phase
*Where in the season this plan fits - base, build, peak, recovery.*

### Outcome
*What the athlete will measurably gain.*

### Notes
*Equipment, altitude, injury considerations, or known conflicts.*

### Copyright
$COPYRIGHT)");

    _description.rangeStart = QDate::currentDate();
    _description.rangeEnd = QDate::currentDate();

#ifdef Q_OS_MAC
    setWizardStyle(QWizard::MacStyle);
#else
    setWizardStyle(QWizard::ModernStyle);
#endif
    setPixmap(ICON_TYPE, svgAsColoredPixmap(":images/breeze/document-export.svg", QSize(ICON_SIZE * dpiXFactor, ICON_SIZE * dpiYFactor), ICON_MARGIN * dpiXFactor, ICON_COLOR));

    setPage(PageSetup, new ExportPlanPageSetup(context, preselectSeason));
    setPage(PageActivities, new ExportPlanPageActivities(context));
    setPage(PageMetadata, new ExportPlanPageMetadata(context));
    setPage(PageSummary, new ExportPlanPageSummary(context));
    setStartId(PageSetup);
}


PlanExportDescription&
ExportPlanWizard::description
()
{
    return _description;
}


void
ExportPlanWizard::updateRange
()
{
    updateRange(description().rangeStart, description().rangeEnd, description().preferOriginal, false);
}


void
ExportPlanWizard::updateRange
(QDate sourceStart, QDate sourceEnd, bool preferOriginal, bool force)
{
    RideCache *const cache =
        context->athlete->rideCache;
    if (   _description.rangeStart != sourceStart
        || _description.rangeEnd != sourceEnd
        || _description.preferOriginal != preferOriginal
        || force) {

        _description.rangeStart = sourceStart;
        _description.rangeEnd = sourceEnd;
        _description.preferOriginal = preferOriginal;

        sourceRides.clear();
        for (RideItem *rideItem : context->athlete->rideCache->rides()) {
            if (   rideItem == nullptr
                || ! rideItem->planned) {
                continue;
            }
            QDate rideDate = PlanBundle::getRideDate(rideItem, preferOriginal);
            if (   rideDate < sourceStart
                || rideDate > sourceEnd) {
                continue;
            }
            sourceRides << SourceRide { rideItem, rideDate, QDate(), true, -1, false };
        }
        std::sort(sourceRides.begin(), sourceRides.end(), [](const SourceRide &a, const SourceRide &b) {
            return a.sourceDate < b.sourceDate;
        });

        // QDateTime of any planned RideItem must be unique
        QHash<QDateTime, int> targetKeyCount;
        for (const SourceRide &sourceRide : sourceRides) {
            RideItem *const item = resolveGuardedRide(
                sourceRide.source, cache);
            if (!item)
                continue;
            QDateTime key(
                sourceRide.sourceDate,
                item->dateTime.time());
            targetKeyCount[key]++;
        }
        QHash<QDateTime, int> keyToGroup;
        int nextGroup = 0;
        for (SourceRide &sourceRide : sourceRides) {
            RideItem *const item = resolveGuardedRide(
                sourceRide.source, cache);
            if (!item)
                continue;
            QDateTime key(
                sourceRide.sourceDate,
                item->dateTime.time());
            if (targetKeyCount[key] > 1) {
                if (! keyToGroup.contains(key)) {
                    keyToGroup[key] = nextGroup++;
                }
                sourceRide.conflictGroup = keyToGroup[key];
            } else {
                sourceRide.conflictGroup = -1;
            }
        }

        QHash<int, bool> groupHasSelection;
        for (SourceRide &sourceRide : sourceRides) {
            if (sourceRide.conflictGroup < 0) {
                continue;
            }
            if (! groupHasSelection.value(sourceRide.conflictGroup, false)) {
                sourceRide.selected = true;
                groupHasSelection[sourceRide.conflictGroup] = true;
            } else {
                sourceRide.selected = false;
            }
        }
    }

    sourceRides.erase(
        std::remove_if(
            sourceRides.begin(), sourceRides.end(),
            [cache](const SourceRide &sourceRide) {
                return !resolveGuardedRide(
                    sourceRide.source, cache);
            }),
        sourceRides.end());

    emit rangeChanged();
}


QString
ExportPlanWizard::expandedDescription
() const
{
    QString text = _description.description.trimmed();
    text.replace("$NAME", _description.name);
    text.replace("$AUTHOR", _description.author);
    text.replace("$SPORT", _description.sport);
    text.replace("$COPYRIGHT", _description.copyright);
    return text;
}


void
ExportPlanWizard::done
(int result)
{
    if (result != QDialog::Accepted) {
        QWizard::done(result);
        return;
    }

    OverrideCursorScope cursorScope;
    QStringList activityFiles;
    bool sourcesValid = true;
    for (const SourceRide &sourceRide : sourceRides) {
        if (! sourceRide.selected || sourceRide.targetBlocked) {
            continue;
        }
        RideItem *const item = resolveGuardedRide(
            sourceRide.source,
            context->athlete->rideCache);
        if (!item) {
            sourcesValid = false;
            break;
        }
        activityFiles << item->fileName;
    }
    if (!sourcesValid) {
        QMessageBox::warning(
            this, tr("Export Plan"),
            tr("An activity changed while the wizard was open. Review the activities and try again."));
        return;
    }

    _description.activityFiles = activityFiles;
    _description.description = expandedDescription();
    if (!runPlanExportCompletion(
            [&] {
                return PlanBundle::exportBundle(
                    context, _description);
            },
            [&] {
                QMessageBox::warning(
                    this, tr("Export Plan"),
                    tr("The plan could not be exported. Check the selected activities and destination, then try again."));
            })) {
        return;
    }

    QWizard::done(result);
}


////////////////////////////////////////////////////////////////////////////////
// ExportPlanPageSetup

ExportPlanPageSetup::ExportPlanPageSetup
(Context *context, Season const * const preselectSeason, QWidget *parent)
: QWizardPage(parent), context(context)
{
    QLocale locale;
    QString localFormat = locale.dateFormat(QLocale::ShortFormat);
    QString customFormat = "ddd, " + localFormat;

    setTitle(tr("Export Plan - Setup"));
    setSubTitle(tr("Define the time range and repetition strategy for exporting activities. Optionally select a season or phase to prefill the dates."));


    QTreeWidget *seasonTree = new QTreeWidget();
    seasonTree->setColumnCount(1);
    basicTreeWidgetStyle(seasonTree, false);
    seasonTree->setHeaderHidden(true);
    seasonTree->resetIndentation();
    QTreeWidgetItem *currentSeason = nullptr;
    for (const Season &season : context->athlete->seasons->seasons) {
        QTreeWidgetItem *seasonItem = new QTreeWidgetItem();
        seasonItem->setData(0, Qt::DisplayRole, season.getName());
        seasonItem->setData(0, Qt::UserRole, season.getStart());
        seasonItem->setData(0, Qt::UserRole + 1, season.getEnd());
        if (preselectSeason != nullptr) {
            if (preselectSeason->id() == season.id()) {
                currentSeason = seasonItem;
            }
        } else if (context->currentSeason() != nullptr) {
            if (context->currentSeason()->id() == season.id()) {
                currentSeason = seasonItem;
            }
        }
        for (const Phase &phase : season.phases) {
            QTreeWidgetItem *phaseItem = new QTreeWidgetItem();
            phaseItem->setData(0, Qt::DisplayRole, phase.getName());
            phaseItem->setData(0, Qt::UserRole, phase.getStart());
            phaseItem->setData(0, Qt::UserRole + 1, phase.getEnd());
            seasonItem->addChild(phaseItem);
            if (preselectSeason != nullptr) {
                if (preselectSeason->id() == phase.id()) {
                    currentSeason = phaseItem;
                }
            } else if (context->currentSeason() != nullptr) {
                if (context->currentSeason()->id() == phase.id()) {
                    currentSeason = phaseItem;
                }
            }
        }
        seasonTree->addTopLevelItem(seasonItem);
    }

    startDate = new QDateEdit();
    startDate->setCalendarPopup(true);
    startDate->setDisplayFormat(customFormat);

    endDate = new QDateEdit();
    endDate->setCalendarPopup(true);
    endDate->setDisplayFormat(customFormat);

    QHBoxLayout *dateRangeLayout = new QHBoxLayout();
    dateRangeLayout->addWidget(startDate);
    dateRangeLayout->addWidget(new QLabel(" - "));
    dateRangeLayout->addWidget(endDate);
    dateRangeLayout->addStretch();

    originalRadio = new QRadioButton(tr("As originally planned"));
    currentRadio = new QRadioButton(tr("As currently planned"));
    QButtonGroup *sourceDateGroup = new QButtonGroup(this);
    sourceDateGroup->setExclusive(true);
    sourceDateGroup->addButton(originalRadio);
    sourceDateGroup->addButton(currentRadio);
    originalRadio->setChecked(true);

    targetRangeBar = new TargetRangeBar(tr("No planned activities in source period"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);

    QVBoxLayout *form = new QVBoxLayout();
    form->addWidget(new QLabel(HLO + tr("Source Period") + HLC));
    form->addWidget(seasonTree);
    form->addLayout(dateRangeLayout);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(new QLabel(HLO + tr("Repetition Strategy") + HLC));
    form->addWidget(originalRadio);
    form->addWidget(currentRadio);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    all->addSpacing(10 * dpiYFactor);
    all->addWidget(targetRangeBar);
    setLayout(all);

    connect(seasonTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current) {
        if (current != nullptr) {
            QDate seasonStart(current->data(0, Qt::UserRole).toDate());
            QDate seasonEnd(current->data(0, Qt::UserRole + 1).toDate());
            startDate->setMaximumDate(seasonEnd);
            endDate->setMinimumDate(seasonStart);

            QSignalBlocker b1(startDate);
            QSignalBlocker b2(endDate);
            startDate->setDate(seasonStart);
            endDate->setDate(seasonEnd);
            refresh();
        }
    });
    connect(startDate, &QDateEdit::dateChanged, this, [this, seasonTree](QDate date) {
        endDate->setMinimumDate(date);
        seasonTree->setCurrentItem(nullptr);
        refresh();
    });
    connect(endDate, &QDateEdit::dateChanged, this, [this, seasonTree](QDate date) {
        startDate->setMaximumDate(date);
        seasonTree->setCurrentItem(nullptr);
        refresh();
    });
    connect(sourceDateGroup, &QButtonGroup::idClicked, this, &ExportPlanPageSetup::refresh);

    if (currentSeason != nullptr) {
        seasonTree->setCurrentItem(currentSeason);
        currentSeason->setSelected(true);
        if (currentSeason->parent() != nullptr) {
            currentSeason->parent()->setExpanded(true);
        }
    } else {
        startDate->setDate(QDate::currentDate().addDays(-7));
        endDate->setDate(QDate::currentDate().addDays(-1));
    }
}


int
ExportPlanPageSetup::nextId
() const
{
    return ExportPlanWizard::PageActivities;
}


void
ExportPlanPageSetup::initializePage
()
{
    targetRangeBar->setFlashEnabled(false);
    refresh();
    targetRangeBar->setFlashEnabled(true);
}


bool
ExportPlanPageSetup::isComplete
() const
{
    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    return wiz->sourceRides.count() > 0;
}


void
ExportPlanPageSetup::refresh
()
{
    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    wiz->updateRange(startDate->date(), endDate->date(), originalRadio->isChecked());

    targetRangeBar->setResult(wiz->description().rangeStart, wiz->description().rangeEnd, wiz->sourceRides.count(), 0);

    emit completeChanged();
}


////////////////////////////////////////////////////////////////////////////////
// ExportPlanPageActivities

ExportPlanPageActivities::ExportPlanPageActivities
(Context *context, QWidget *parent)
: QWizardPage(parent), context(context)
{
    setTitle(tr("Export Plan - Activities"));
    setSubTitle(tr("Review and choose the activities you wish to export."));


    setFinalPage(false);

    activityTree = new QTreeWidget();
    activityTree->setHeaderHidden(true);
    activityTree->setColumnCount(3);
    activityTree->setRootIsDecorated(false);
    activityTree->setSelectionMode(QAbstractItemView::SingleSelection);
    activityTree->setItemDelegate(new IndicatorDelegate());
    activityTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

    targetRangeBar = new TargetRangeBar(tr("No selected activities"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);

    QWidget *formWidget = new QWidget();
    QVBoxLayout *form = new QVBoxLayout(formWidget);
    form->addWidget(new QLabel(HLO + tr("Activities for your export") + HLC));
    form->addWidget(activityTree);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    form->addSpacing(10 * dpiYFactor);
    all->addWidget(targetRangeBar);
    setLayout(all);
}


int
ExportPlanPageActivities::nextId
() const
{
    return ExportPlanWizard::PageMetadata;
}


void
ExportPlanPageActivities::initializePage
()
{
    disconnect(dataChangedConnection);
    numSelected = 0;
    activityTree->clear();

    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }
    targetRangeBar->setFlashEnabled(false);
    wiz->updateRange();

    QLocale locale;
    int i = 0;
    while (i < wiz->sourceRides.count()) {
        const SourceRide &sourceRide = wiz->sourceRides[i];
        RideItem *const sourceItem =
            resolveGuardedRide(
                sourceRide.source,
                context->athlete->rideCache);
        if (!sourceItem) {
            ++i;
            continue;
        }

        if (sourceRide.conflictGroup < 0) {
            QTreeWidgetItem *item = new QTreeWidgetItem();
            item->setData(0, Qt::DisplayRole, locale.toString(sourceRide.sourceDate, QLocale::ShortFormat));
            item->setData(0, IndicatorDelegate::IndicatorTypeRole, IndicatorDelegate::CheckIndicator);
            item->setData(0, IndicatorDelegate::IndicatorStateRole, sourceRide.selected);
            item->setData(0, IndexRole, i);
            item->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(sourceItem));
            item->setData(2, Qt::DisplayRole, PlanBundle::getRideName(sourceItem));
            activityTree->addTopLevelItem(item);

            if (sourceRide.selected) {
                ++numSelected;
            }
            ++i;
        } else {
            int group = sourceRide.conflictGroup;
            QList<int> groupIndices;
            while (i < wiz->sourceRides.count() && wiz->sourceRides[i].conflictGroup == group) {
                groupIndices << i;
                ++i;
            }

            // Parent node — warning icon, shared target date, no checkbox
            QTreeWidgetItem *groupItem = new QTreeWidgetItem(activityTree);
            groupItem->setFirstColumnSpanned(true);
            groupItem->setIcon(0, style()->standardIcon(QStyle::SP_MessageBoxWarning));
            groupItem->setData(0, Qt::DisplayRole, tr("Choose one to export"));
            groupItem->setExpanded(true);

            for (int idx : groupIndices) {
                const SourceRide &groupRide = wiz->sourceRides[idx];
                RideItem *const groupItemRide =
                    resolveGuardedRide(
                        groupRide.source,
                        context->athlete->rideCache);
                if (!groupItemRide)
                    continue;
                QTreeWidgetItem *child = new QTreeWidgetItem(groupItem);
                child->setData(0, Qt::DisplayRole, locale.toString(groupRide.sourceDate, QLocale::ShortFormat));
                child->setData(0, IndicatorDelegate::IndicatorTypeRole, IndicatorDelegate::RadioIndicator);
                child->setData(0, IndicatorDelegate::IndicatorStateRole, groupRide.selected);
                child->setData(0, IndexRole, idx);
                child->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(groupItemRide));
                child->setData(2, Qt::DisplayRole, PlanBundle::getRideName(groupItemRide));
            }
            ++numSelected;
        }
    }

    dataChangedConnection = connect(activityTree->model(), &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &index) {
        ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
        if (wiz == nullptr) {
            return;
        }
        QModelIndex col0Index = index.siblingAtColumn(0);
        int indicatorType = col0Index.data(IndicatorDelegate::IndicatorTypeRole).toInt();
        if (indicatorType == IndicatorDelegate::NoIndicator) {
            return;
        }
        int i = col0Index.data(IndexRole).toInt();
        if (i < 0 || i >= wiz->sourceRides.size()) {
            return;
        }
        if (!resolveGuardedRide(
                wiz->sourceRides[i].source,
                context->athlete->rideCache)) {
            wiz->updateRange();
            initializePage();
            return;
        }
        bool checked = col0Index.data(IndicatorDelegate::IndicatorStateRole).toBool();
        wiz->sourceRides[i].selected = checked;
        if (indicatorType == IndicatorDelegate::CheckIndicator) {
            numSelected += checked ? 1 : -1;
            emit completeChanged();
        }
        const int previousSourceCount =
            wiz->sourceRides.size();
        wiz->updateRange();
        if (wiz->sourceRides.size()
            != previousSourceCount) {
            initializePage();
            return;
        }
        targetRangeBar->setResult(wiz->description().rangeStart, wiz->description().rangeEnd, numSelected, 0);
    });
    targetRangeBar->setResult(wiz->description().rangeStart, wiz->description().rangeEnd, numSelected, 0);
    targetRangeBar->setFlashEnabled(true);
}


bool
ExportPlanPageActivities::isComplete
() const
{
    return numSelected > 0;
}


void
ExportPlanPageActivities::cleanupPage
()
{
    disconnect(dataChangedConnection);
}


////////////////////////////////////////////////////////////////////////////////
// ExportPlanPageMetadata

ExportPlanPageMetadata::ExportPlanPageMetadata
(Context *context, QWidget *parent)
: QWizardPage(parent), context(context)
{
    setTitle(tr("Export Plan - Metadata"));
    setSubTitle(tr("Provide a description for the exported content. This will be shown again when importing."));

    nameEdit = new QLineEdit();
    authorEdit = new QLineEdit();
    copyrightEdit = new QLineEdit();
    descriptionEdit = new QTextEdit();
    descriptionEdit->setPlaceholderText(tr("Describe your plan in markdown..."));

    QFormLayout *form = newQFormLayout();
    form->addRow(tr("Plan name") + "*", nameEdit);
    form->addRow(tr("Author") + "*", authorEdit);
    form->addRow(tr("Copyright"), copyrightEdit);
    form->addRow(tr("Description"), descriptionEdit);

    targetRangeBar = new TargetRangeBar(tr("No selected activities"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);
    targetRangeBar->setFlashEnabled(false);

    QVBoxLayout *all = new QVBoxLayout();
    all->addLayout(form);
    all->addSpacing(10 * dpiYFactor);
    all->addWidget(targetRangeBar);
    setLayout(all);

    connect(authorEdit, &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
    connect(nameEdit, &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
}


int
ExportPlanPageMetadata::nextId
() const
{
    return ExportPlanWizard::PageSummary;
}


void
ExportPlanPageMetadata::initializePage
()
{
    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    authorEdit->setText(wiz->description().author);
    nameEdit->setText(wiz->description().name);
    copyrightEdit->setText(wiz->description().copyright);
    descriptionEdit->setText(wiz->description().description);

    int numSelected = 0;
    for (const SourceRide &sourceRide : wiz->sourceRides) {
        if (! sourceRide.selected) {
            continue;
        }
        ++numSelected;
    }
    targetRangeBar->setResult(wiz->description().rangeStart, wiz->description().rangeEnd, numSelected, 0);
}


bool
ExportPlanPageMetadata::validatePage
()
{
    cleanupPage();
    return true;
}


bool
ExportPlanPageMetadata::isComplete
() const
{
    return    ! authorEdit->text().trimmed().isEmpty()
           && ! nameEdit->text().trimmed().isEmpty();
}


void
ExportPlanPageMetadata::cleanupPage
()
{
    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }
    wiz->description().author = authorEdit->text().trimmed();
    wiz->description().name = nameEdit->text().trimmed();
    wiz->description().copyright = copyrightEdit->text().trimmed();
    wiz->description().description = descriptionEdit->toPlainText().trimmed();
}


////////////////////////////////////////////////////////////////////////////////
// ExportPlanPageSummary

ExportPlanPageSummary::ExportPlanPageSummary
(Context *context, QWidget *parent)
: QWizardPage(parent), context(context)
{
    setTitle(tr("Export Plan - Summary"));
    setSubTitle(tr("Preview the export results, including all selected activities. No data will be exported until you continue."));

    setFinalPage(true);

    nameBox = new DisplayBox(tr("Plan name"), DisplayBox::Size::Large);
    authorBox = new DisplayBox(tr("Author"), DisplayBox::Size::Medium);
    sportBox = new DisplayBox(tr("Sport"), DisplayBox::Size::Small, true);
    durationBox = new DisplayBox(tr("Duration"), DisplayBox::Size::Small);
    countBox = new DisplayBox(tr("Activities"), DisplayBox::Size::Small);
    copyrightBox = new DisplayBox(tr("Copyright"), DisplayBox::Size::Tiny);

    descriptionValue = new QLabel();
    descriptionValue->setTextFormat(Qt::RichText);
    descriptionValue->setWordWrap(true);
    descriptionValue->setTextInteractionFlags(Qt::NoTextInteraction);
    QPalette descPal = descriptionValue->palette();
    descPal.setColor(QPalette::Window, descPal.color(QPalette::Base));
    descriptionValue->setAutoFillBackground(true);
    descriptionValue->setPalette(descPal);

    QWidget *overviewFieldsWidget = new QWidget();

    int row = 0;
    QGridLayout *overviewFieldsLayout = new QGridLayout(overviewFieldsWidget);
    overviewFieldsLayout->setColumnStretch(0, 6);
    overviewFieldsLayout->setColumnStretch(1, 4);
    overviewFieldsLayout->setContentsMargins(0, 0, 0, 0);
    overviewFieldsLayout->addWidget(nameBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->addWidget(authorBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->addWidget(sportBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->addWidget(durationBox, row, 0);
    overviewFieldsLayout->addWidget(countBox, row, 1);
    ++row;
    overviewFieldsLayout->addWidget(copyrightBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->setRowStretch(row, 1);

    QScrollArea *overviewFieldsScroll = new QScrollArea();
    overviewFieldsScroll->setFrameShape(QFrame::NoFrame);
    overviewFieldsScroll->setWidget(overviewFieldsWidget);
    overviewFieldsScroll->setWidgetResizable(true);
    overviewFieldsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewFieldsScroll->setMinimumWidth(350 * dpiXFactor);
    overviewFieldsScroll->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    QScrollArea *overviewDescScroll = new QScrollArea();
    overviewDescScroll->setFrameShape(QFrame::NoFrame);
    overviewDescScroll->setWidget(descriptionValue);
    overviewDescScroll->setWidgetResizable(true);
    overviewDescScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewDescScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QFrame *fieldsFrame = new QFrame();
    fieldsFrame->setContentsMargins(10 * dpiXFactor, 30 * dpiYFactor, 0, 10 * dpiYFactor);

    QVBoxLayout *fieldsLayout = new QVBoxLayout(fieldsFrame);
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    fieldsLayout->addWidget(new QLabel(tr("PLAN DETAILS")), 0);
    fieldsLayout->addWidget(overviewFieldsScroll, 1);

    QFrame *descFrame = new QFrame();
    descFrame->setContentsMargins(15 * dpiXFactor, 30 * dpiYFactor, 0, 10 * dpiYFactor);
    descFrame->setAutoFillBackground(true);
    descFrame->setPalette(descPal);

    QVBoxLayout *descLayout = new QVBoxLayout(descFrame);
    descLayout->setContentsMargins(0, 0, 0, 0);
    descLayout->addWidget(new QLabel(tr("PLAN DESCRIPTION")), 0);
    descLayout->addWidget(overviewDescScroll, 1);

    QWidget *overviewWidget = new QWidget();
    overviewWidget->setAutoFillBackground(true);

    QHBoxLayout *overviewSplitLayout = new QHBoxLayout(overviewWidget);
    overviewSplitLayout->setContentsMargins(0, 0, 0, 0);
    overviewSplitLayout->addWidget(fieldsFrame, 0);
    overviewSplitLayout->addSpacing(10 * dpiXFactor);
    overviewSplitLayout->addWidget(descFrame, 1);

    planTree = new QTreeWidget();
    planTree->setColumnCount(4);
    planTree->setHeaderLabels({ tr("Week"), tr("Day"), tr("Sport"), tr("Name") });
    basicTreeWidgetStyle(planTree, false);
    planTree->setHeaderHidden(false);

    outputPathWidget = new DirectoryPathWidget(DirectoryPathMode::FileSave);
    outputPathWidget->setNameFilter(tr("Golden Cheetah Plans") + QString(" (*%1)").arg(planExtension));
    outputPathWidget->setPlaceholderText(tr("Output file"));

    QTabWidget *tabWidget = new QTabWidget();
    tabWidget->addTab(overviewWidget, tr("Overview"));
    tabWidget->addTab(planTree, tr("Activities in export"));

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(tabWidget);
    all->addSpacing(10 * dpiYFactor);
    all->addWidget(outputPathWidget);
    setLayout(all);

    connect(outputPathWidget, &DirectoryPathWidget::pathChanged, this, [this](QString path) {
        ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
        if (wiz == nullptr) {
            return;
        }
        if (path != wiz->description().planFile) {
            wiz->description().planFile = path.trimmed();
            emit completeChanged();
        }
    });
}


int
ExportPlanPageSummary::nextId
() const
{
    return ExportPlanWizard::Finalize;
}


void
ExportPlanPageSummary::initializePage
()
{
    planTree->clear();

    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    QDate dayOne = wiz->description().rangeStart;

    QLocale locale;
    int numSelected = 0;
    QSet<QString> sports;
    for (const SourceRide &sourceRide : wiz->sourceRides) {
        if (! sourceRide.selected) {
            continue;
        }
        RideItem *const sourceItem =
            resolveGuardedRide(
                sourceRide.source,
                context->athlete->rideCache);
        if (!sourceItem)
            continue;
        QString sport =
            PlanBundle::getRideSport(sourceItem);
        QTreeWidgetItem *planItem = new QTreeWidgetItem(planTree);
        int day = dayOne.daysTo(sourceRide.sourceDate);
        planItem->setData(0, Qt::DisplayRole, QString::number(day / 7 + 1));
        planItem->setData(1, Qt::DisplayRole, QString::number(day % 7 + 1));
        planItem->setData(2, Qt::DisplayRole, sport);
        planItem->setData(3, Qt::DisplayRole, PlanBundle::getRideName(sourceItem));
        ++numSelected;
        sports.insert(sport);
    }
    wiz->description().sport = sports.values().join(", ");

    bool showAuthor = ! wiz->description().author.trimmed().isEmpty();
    if (showAuthor) {
        authorBox->setText(wiz->description().author.trimmed());
    }
    authorBox->setVisible(showAuthor);

    bool showName = ! wiz->description().name.trimmed().isEmpty();
    if (showName) {
        nameBox->setText(wiz->description().name.trimmed());
    }
    nameBox->setVisible(showName);

    bool showCopyright = ! wiz->description().copyright.trimmed().isEmpty();
    if (showCopyright) {
        copyrightBox->setText(wiz->description().copyright.trimmed());
    }
    copyrightBox->setVisible(showCopyright);

    bool showDescription = ! wiz->description().description.trimmed().isEmpty();
    if (showDescription) {
        QTextDocument doc;
        doc.setMarkdown(wiz->expandedDescription());
        descriptionValue->setText(doc.toHtml());
    }
    descriptionValue->setVisible(showDescription);

    int days = wiz->description().rangeStart.daysTo(wiz->description().rangeEnd) + 1;
    if (days == 1) {
        durationBox->setText(tr("1 day"));
    } else if (days < 14) {
        durationBox->setText(tr("%1 days").arg(days));
    } else {
        durationBox->setText(tr("%1 weeks").arg((days + 6) / 7));
    }

    sportBox->setText(wiz->description().sport);
    countBox->setText(QString("%1").arg(numSelected));

    if (wiz->description().planFile.trimmed().isEmpty()) {
        QDir dir = QDir::home();
        QString filename = sanitizeFilename(wiz->description().name.trimmed());
        if (filename.isEmpty()) {
            filename = "plan";
            QFile file(dir.absoluteFilePath(filename + planExtension));
            int i = 0;
            while (file.exists()) {
                ++i;
                filename = QString("plan_%1").arg(i);
                file.setFileName(dir.absoluteFilePath(filename + planExtension));
            }
        }
        filename += planExtension;
        wiz->description().planFile = dir.absoluteFilePath(filename);
    }
    outputPathWidget->setPath(wiz->description().planFile);
}


bool
ExportPlanPageSummary::isComplete
() const
{
    ExportPlanWizard *wiz = qobject_cast<ExportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }
    if (! wiz->description().planFile.trimmed().isEmpty()) {
        QFileInfo fi(wiz->description().planFile.trimmed());
        QDir dir(fi.absolutePath());
        bool ret = true;
        if (   ! dir.exists()
            || fi.isDir()
            || (fi.exists() && ! fi.isWritable())) {
            ret =  false;
        } else {
            QTemporaryFile tmp(dir.filePath("XXXXXX"));
            ret = tmp.open();
        }
        return ret;
    } else {
        return false;
    }
}


QString
ExportPlanPageSummary::sanitizeFilename
(QString input) const
{
    QString name = input.trimmed();
    name.replace(' ', '_');
    QRegularExpression invalid(R"([^\p{L}\p{N}._-])");
    name.replace(invalid, "_");
    name.replace(QRegularExpression("_+"), "_");
    name.remove(QRegularExpression(R"(^[._-]+)"));
    name.remove(QRegularExpression(R"([._-]+$)"));
    static const QStringList reserved = {
        "CON","PRN","AUX","NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"
    };
    if (reserved.contains(name.toUpper())) {
        name.prepend("_");
    }
    const int maxLength = 200;
    if (name.length() > maxLength) {
        name = name.left(maxLength);
    }
    return name;
}


////////////////////////////////////////////////////////////////////////////////
// ImportPlanWizard

ImportPlanWizard::ImportPlanWizard
(Context *context, QDate targetDay, QWidget *parent)
: QWizard(parent),
  planReader(QSharedPointer<PlanBundleReader>::create(
      context, targetDay))
{
    setWindowTitle(tr("Import Plan"));
    setMinimumSize(800 * dpiXFactor, 750 * dpiYFactor);
    setModal(true);

#ifdef Q_OS_MAC
    setWizardStyle(QWizard::MacStyle);
#else
    setWizardStyle(QWizard::ModernStyle);
#endif
    setPixmap(ICON_TYPE, svgAsColoredPixmap(":images/breeze/document-import.svg", QSize(ICON_SIZE * dpiXFactor, ICON_SIZE * dpiYFactor), ICON_MARGIN * dpiXFactor, ICON_COLOR));

    setOption(QWizard::NoCancelButtonOnLastPage, true);

    setPage(PageSetup, new ImportPlanPageSetup());
    setPage(PageActivities, new ImportPlanPageActivities());
    setPage(PageSummary, new ImportPlanPageSummary());
    setPage(PageResult, new ImportPlanPageResult());
    setStartId(PageSetup);
}


////////////////////////////////////////////////////////////////////////////////
// ImportPlanPageSetup

ImportPlanPageSetup::ImportPlanPageSetup
(QWidget *parent)
: QWizardPage(parent)
{
    setTitle(tr("Import Plan - Setup"));
    setSubTitle(tr("Define the target time range for placing the activities."));

    DirectoryPathWidget *inputPathWidget = new DirectoryPathWidget(DirectoryPathMode::FileOpen);
    inputPathWidget->setNameFilter(tr("Golden Cheetah Plans") + QString(" (*%1)").arg(planExtension));
    inputPathWidget->setPlaceholderText(tr("Plan bundle"));

    targetRangeBar = new TargetRangeBar(tr("No planned activities in source period"));
    targetRangeBar->setCopyMsg(tr("%1 to import"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);

    gapDayCheck = new QCheckBox(tr("Keep leading and trailing gaps"));

    statusLabel = new QLabel();
    statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusFrame = new QFrame();
    statusFrame->setFrameShape(QFrame::StyledPanel);
    statusFrame->setFrameShadow(QFrame::Plain);
    statusFrame->setLineWidth(1 * dpiXFactor);
    QHBoxLayout *statusLayout = new QHBoxLayout(statusFrame);
    statusLayout->setContentsMargins(8 * dpiXFactor, 15 * dpiYFactor, 8 * dpiXFactor, 4 * dpiYFactor);
    statusLayout->setSpacing(6 * dpiXFactor);
    statusLayout->addWidget(statusLabel);

    nameBox = new DisplayBox(tr("Plan name"), DisplayBox::Size::Large);
    authorBox = new DisplayBox(tr("Author"), DisplayBox::Size::Medium);
    sportBox = new DisplayBox(tr("Sport"), DisplayBox::Size::Small, true);
    durationBox = new DisplayBox(tr("Duration"), DisplayBox::Size::Small);
    countBox = new DisplayBox(tr("Activities"), DisplayBox::Size::Small);
    copyrightBox = new DisplayBox(tr("Copyright"), DisplayBox::Size::Tiny);

    QFrame *separator = new QFrame();
    QColor sepCol = separator->palette().color(QPalette::Mid);
    sepCol.setAlpha(180);
    QString style = QString("background-color: %1;").arg(sepCol.name(QColor::HexArgb));
    separator->setFixedHeight(1);
    separator->setStyleSheet(style);

    descriptionValue = new QLabel();
    descriptionValue->setTextFormat(Qt::RichText);
    descriptionValue->setTextInteractionFlags(Qt::NoTextInteraction);
    descriptionValue->setWordWrap(true);
    descriptionValue->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QPalette descPal = descriptionValue->palette();
    descPal.setColor(QPalette::Window, descPal.color(QPalette::Base));
    descriptionValue->setAutoFillBackground(true);
    descriptionValue->setPalette(descPal);

    QWidget *overviewFieldsWidget = new QWidget();

    int row = 0;
    QGridLayout *overviewFieldsLayout = new QGridLayout(overviewFieldsWidget);
    overviewFieldsLayout->setColumnStretch(0, 6);
    overviewFieldsLayout->setColumnStretch(1, 4);
    overviewFieldsLayout->setContentsMargins(0, 0, 0, 0);
    overviewFieldsLayout->addWidget(nameBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->addWidget(authorBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->addWidget(sportBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->addWidget(durationBox, row, 0);
    overviewFieldsLayout->addWidget(countBox, row, 1);
    ++row;
    overviewFieldsLayout->addWidget(copyrightBox, row, 0, 1, 2);
    ++row;
    overviewFieldsLayout->setRowStretch(row, 1);

    QScrollArea *overviewFieldsScroll = new QScrollArea();
    overviewFieldsScroll->setFrameShape(QFrame::NoFrame);
    overviewFieldsScroll->setWidget(overviewFieldsWidget);
    overviewFieldsScroll->setWidgetResizable(true);
    overviewFieldsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewFieldsScroll->setMinimumWidth(350 * dpiXFactor);
    overviewFieldsScroll->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    QScrollArea *overviewDescScroll = new QScrollArea();
    overviewDescScroll->setFrameShape(QFrame::NoFrame);
    overviewDescScroll->setWidget(descriptionValue);
    overviewDescScroll->setWidgetResizable(true);
    overviewDescScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewDescScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QFrame *fieldsFrame = new QFrame();
    fieldsFrame->setContentsMargins(0, 0, 0, 0);

    QVBoxLayout *fieldsLayout = new QVBoxLayout(fieldsFrame);
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    fieldsLayout->addWidget(new QLabel(tr("PLAN DETAILS")), 0);
    fieldsLayout->addWidget(overviewFieldsScroll, 1);

    QFrame *descFrame = new QFrame();
    fieldsFrame->setContentsMargins(0, 0, 0, 0);
    descFrame->setAutoFillBackground(true);
    descFrame->setPalette(descPal);

    QVBoxLayout *descLayout = new QVBoxLayout(descFrame);
    descLayout->setContentsMargins(15 * dpiXFactor, 0, 0, 0);
    descLayout->addWidget(new QLabel(tr("PLAN DESCRIPTION")), 0);
    descLayout->addWidget(overviewDescScroll, 1);

    QHBoxLayout *overviewSplitLayout = new QHBoxLayout();
    overviewSplitLayout->setContentsMargins(0, 30 * dpiYFactor, 0, 0);
    overviewSplitLayout->addWidget(fieldsFrame, 0);
    overviewSplitLayout->addSpacing(10 * dpiXFactor);
    overviewSplitLayout->addWidget(descFrame, 1);

    QVBoxLayout *overviewLayout = new QVBoxLayout();
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    overviewLayout->addWidget(gapDayCheck, 0);
    overviewLayout->addWidget(separator, 0);
    overviewLayout->addWidget(statusFrame, 0);
    overviewLayout->addLayout(overviewSplitLayout, 1);
    overviewLayout->addSpacing(10 * dpiYFactor);
    overviewLayout->addWidget(targetRangeBar, 0);

    QWidget *overviewWidget = new QWidget();
    overviewWidget->setLayout(overviewLayout);

    overviewError = new QLabel();
    overviewError->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    overviewError->setContentsMargins(0, 30 * dpiYFactor, 0, 30 * dpiYFactor);
    overviewEmpty = new QLabel();
    overviewEmpty->setContentsMargins(0, 30 * dpiYFactor, 0, 30 * dpiYFactor);
    overviewEmpty->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    overviewEmpty->setText(  QString("<div style='line-height: 1.3;'>")
                           + "<h4>" + tr("No plan loaded") + "</h4>"
                           + tr("Select a training plan file (%1) to begin").arg(planExtension)
                           + "</div>");

    overviewStack = new QStackedWidget();
    overviewStack->setContentsMargins(0, 0, 0, 0);
    overviewStack->addWidget(overviewWidget);
    overviewStack->addWidget(overviewEmpty);
    overviewStack->addWidget(overviewError);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(inputPathWidget);
    all->addWidget(overviewStack);
    setLayout(all);

    overviewStack->setCurrentIndex(1);

    connect(inputPathWidget, &DirectoryPathWidget::pathChanged, this, &ImportPlanPageSetup::bundlePathChanged);
    connect(gapDayCheck, &QCheckBox::toggled, this, [this](bool checked) {
        ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
        if (wiz == nullptr || !wiz->planReader) {
            return;
        }
        wiz->planReader->setIncludeGapDays(checked);
        updateRanges();
    });
}


int
ImportPlanPageSetup::nextId
() const
{
    return ImportPlanWizard::PageActivities;
}


bool
ImportPlanPageSetup::isComplete
() const
{
    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }
    return wiz->planReader && wiz->planReader->isValid();
}


void
ImportPlanPageSetup::bundlePathChanged
(QString path)
{
    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader) {
        return;
    }

    gapDayCheck->setChecked(
        wiz->planReader->isIncludeGapDays());

    PlanResult result =
        wiz->planReader->loadBundle(path.trimmed());

    if (path.trimmed().isEmpty()) {
        overviewStack->setCurrentIndex(1);
    } else if (! result.ok()) {
        overviewStack->setCurrentIndex(2);

        QString errorMessage;
        errorMessage = HLO + tr("Failed to load plan bundle.") + HLC + "<br>";
        errorMessage += tr("The selected file is not a valid training plan or is corrupted.");
        errorMessage += QString("<br><br>") + HLO + tr("Errors") + HLC;
        errorMessage += "<ul>";
        for (const QString &error : result.errors) {
            errorMessage += "<li>" + error + "</li>";
        }
        errorMessage += "</ul>";
        if (! result.warnings.isEmpty()) {
            errorMessage += QString("<br>") + HLO + tr("Warnings") + HLC;
            errorMessage += "<ul>";
            for (const QString &warning : result.warnings) {
                errorMessage += "<li>" + warning + "</li>";
            }
            errorMessage += "</ul>";
        }
        overviewError->setText(errorMessage);
    } else if (wiz->planReader->isValid()) {
        overviewStack->setCurrentIndex(0);

        QString statusMessage;
        if (! result.warnings.isEmpty()) {
            statusMessage = HLO + tr("The plan was loaded with some warnings.") + HLC + "<br>";
            statusMessage += tr("Some items may be incomplete or have been adjusted.");
            statusMessage += "<ul>";
            for (const QString &warning : result.warnings) {
                statusMessage += "<li>" + warning + "</li>";
            }
            statusMessage += "</ul>";
            statusFrame->setVisible(true);
        } else {
            statusFrame->setVisible(false);
        }
        statusLabel->setText(statusMessage);

        authorBox->setText(
            wiz->planReader->getMetadata().author);
        nameBox->setText(
            wiz->planReader->getMetadata().name);
        updateRanges();
        sportBox->setText(
            wiz->planReader->getMetadata().sport);
        countBox->setText(QString::number(
            wiz->planReader->getNumActivities()));
        copyrightBox->setText(
            wiz->planReader->getMetadata().copyright);
        copyrightBox->setVisible(
            !wiz->planReader->getMetadata().copyright.isEmpty());

        bool showDescription = !wiz->planReader
            ->getMetadata().description.trimmed().isEmpty();
        if (showDescription) {
            QTextDocument doc;
            doc.setMarkdown(
                wiz->planReader->getMetadata().description);
            descriptionValue->setText(doc.toHtml());
        }
        descriptionValue->setVisible(showDescription);
    }
    emit completeChanged();
}


void
ImportPlanPageSetup::updateRanges
()
{
    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader
        || !wiz->planReader->isValid()) {
        return;
    }

    int days = wiz->planReader->getDuration();
    if (days == 1) {
        durationBox->setText(tr("1 day"));
    } else if (days < 14) {
        durationBox->setText(tr("%1 days").arg(days));
    } else {
        durationBox->setText(tr("%1 weeks").arg((days + 6) / 7));
    }

    targetRangeBar->setResult(
        wiz->planReader->getTargetRangeStart(),
        wiz->planReader->getTargetRangeEnd(),
        wiz->planReader->getNumActivities(),
        wiz->planReader->getActivitiesToRemove().count());
}


////////////////////////////////////////////////////////////////////////////////
// ImportPlanPageActivities

ImportPlanPageActivities::ImportPlanPageActivities
(QWidget *parent)
: QWizardPage(parent)
{
    setTitle(tr("Import Plan - Activities"));
    setSubTitle(tr("Review and choose the activities you wish to import."));

    setFinalPage(false);

    activityTree = new QTreeWidget();
    activityTree->setHeaderHidden(true);
    activityTree->setColumnCount(3);
    activityTree->setRootIsDecorated(false);
    activityTree->setSelectionMode(QAbstractItemView::SingleSelection);
    activityTree->setItemDelegate(new IndicatorDelegate());
    activityTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

    targetRangeBar = new TargetRangeBar(tr("No selected activities"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);

    QWidget *formWidget = new QWidget();
    QVBoxLayout *form = new QVBoxLayout(formWidget);
    form->addWidget(new QLabel(HLO + tr("Activities to import") + HLC));
    form->addWidget(activityTree);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    form->addSpacing(10 * dpiYFactor);
    all->addWidget(targetRangeBar);
    setLayout(all);
}


int
ImportPlanPageActivities::nextId
() const
{
    return ImportPlanWizard::PageSummary;
}


void
ImportPlanPageActivities::initializePage
()
{
    disconnect(dataChangedConnection);

    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader
        || !wiz->planReader->isValid()) {
        return;
    }

    QLocale locale;
    QString localFormat = locale.dateFormat(QLocale::ShortFormat);
    QString customFormat = "ddd, " + localFormat;

    bool selectionChanged = false;
    do {
        selectionChanged = false;
        for (int index = 0;
             index < wiz->planReader->rideFiles.size();
             ++index) {
            const RideFileSelection &selection =
                wiz->planReader->rideFiles[index];
            const PlanImportScheduleActivity activity {
                selection.getRideFile()->startTime(),
                selection.selected};
            if (planImportHasLinkedConflict(
                    activity, selection.targetDateTime,
                    wiz->planReader->getExistingLinked())) {
                wiz->planReader->setActivitySelected(
                    index, false);
                selectionChanged = true;
                break;
            }
        }
    } while (selectionChanged);

    activityTree->clear();

    for (int i = 0;
         i < wiz->planReader->rideFiles.size(); ++i) {
        RideFileSelection &rfs =
            wiz->planReader->rideFiles[i];

        QTreeWidgetItem *item = new QTreeWidgetItem(activityTree);

        QDateTime targetDate = rfs.targetDateTime;
        item->setData(0, Qt::DisplayRole, locale.toString(targetDate, customFormat));
        item->setData(0, IndicatorDelegate::IndicatorTypeRole, IndicatorDelegate::CheckIndicator);
        item->setData(0, IndexRole, i);
        item->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(rfs.getRideFile()));
        item->setData(2, Qt::DisplayRole, PlanBundle::getRideName(rfs.getRideFile()));

        if (wiz->planReader->getExistingLinked()
                .contains(targetDate)) {
            item->setToolTip(0, tr("Conflicts with existing planned and linked activity"));
            item->setToolTip(1, tr("Conflicts with existing planned and linked activity"));
            item->setToolTip(2, tr("Conflicts with existing planned and linked activity"));
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled & ~Qt::ItemIsUserCheckable);
        }
        item->setData(0, IndicatorDelegate::IndicatorStateRole, rfs.selected);
    }

    dataChangedConnection = connect(activityTree->model(), &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &index) {
        ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
        if (wiz == nullptr || !wiz->planReader) {
            return;
        }
        QModelIndex col0Index = index.siblingAtColumn(0);
        int indicatorType = col0Index.data(IndicatorDelegate::IndicatorTypeRole).toInt();
        if (indicatorType == IndicatorDelegate::NoIndicator) {
            return;
        }
        int i = col0Index.data(IndexRole).toInt();
        if (i < 0
            || i >= wiz->planReader->rideFiles.size()) {
            return;
        }
        bool checked = col0Index.data(IndicatorDelegate::IndicatorStateRole).toBool();
        if (!wiz->planReader->setActivitySelected(
                i, checked)) {
            return;
        }
        if (indicatorType == IndicatorDelegate::CheckIndicator) {
            emit completeChanged();
        }
        initializePage();
    });
    targetRangeBar->setResult(
        wiz->planReader->getTargetRangeStart(),
        wiz->planReader->getTargetRangeEnd(),
        wiz->planReader->getNumSelectedActivities(),
        wiz->planReader->getActivitiesToRemove().count());
    targetRangeBar->setFlashEnabled(true);
    emit completeChanged();
}


bool
ImportPlanPageActivities::isComplete
() const
{
    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader) {
        return false;
    }
    return wiz->planReader->getNumSelectedActivities() > 0;
}


void
ImportPlanPageActivities::cleanupPage
()
{
    disconnect(dataChangedConnection);
}


////////////////////////////////////////////////////////////////////////////////
// ImportPlanPageSummary

ImportPlanPageSummary::ImportPlanPageSummary
(QWidget *parent)
: QWizardPage(parent)
{
    setTitle(tr("Import Plan - Summary"));
    setSubTitle(tr("Preview the plan updates, including planned additions and deletions. No changes will be made until you continue."));

    setCommitPage(true);

    planLabel = new QLabel(HLO + tr("New Plan Overview") + HLC);
    planTree = new QTreeWidget();
    planTree->setColumnCount(5);
    basicTreeWidgetStyle(planTree, false);
    planTree->setHeaderHidden(true);

    deletionLabel = new QLabel(HLO + tr("Planned Activities Marked for Deletion") + HLC);
    deletionTree = new QTreeWidget();
    deletionTree->setColumnCount(3);
    basicTreeWidgetStyle(deletionTree, false);
    deletionTree->setHeaderHidden(true);

    targetRangeBar = new TargetRangeBar(tr("No selected activities"));
    targetRangeBar->setMinimumWidth(650 * dpiXFactor);
    targetRangeBar->setFlashEnabled(false);

    QWidget *formWidget = new QWidget();
    QVBoxLayout *form = new QVBoxLayout(formWidget);
    form->addWidget(planLabel);
    form->addWidget(planTree);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(deletionLabel);
    form->addWidget(deletionTree);
    form->addSpacing(10 * dpiYFactor);
    form->addWidget(targetRangeBar);

    QWidget *scrollWidget = new QWidget();
    scrollWidget->setLayout(form);
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);

    QVBoxLayout *all = new QVBoxLayout();
    all->addWidget(scrollArea);
    setLayout(all);
}


int
ImportPlanPageSummary::nextId
() const
{
    return ImportPlanWizard::PageResult;
}


void
ImportPlanPageSummary::initializePage
()
{
    planTree->clear();
    deletionTree->clear();

    deletionLabel->setVisible(false);
    deletionTree->setVisible(false);

    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader) {
        return;
    }

    QLocale locale;
    int numSelected = 0;
    for (const RideFileSelection &rideFile :
         wiz->planReader->rideFiles) {
        QTreeWidgetItem *planItem = new QTreeWidgetItem(planTree);
        planItem->setData(0, Qt::DisplayRole, locale.toString(rideFile.targetDateTime.date(), QLocale::ShortFormat));
        if (! rideFile.selected) {
            QFont font;
            font.setItalic(true);
            planItem->setData(0, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(1, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(2, Qt::ForegroundRole, palette().color(QPalette::Disabled, QPalette::Text));
            planItem->setData(2, Qt::FontRole, font);
            planItem->setData(2, Qt::DisplayRole, tr("skipped"));
        }
        planItem->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(rideFile.getRideFile()));
        planItem->setData(2, Qt::DisplayRole, PlanBundle::getRideName(rideFile.getRideFile()));
    }
    int deletionCount = 0;
    for (RideItem *rideItem :
         wiz->planReader->getRideItemsToRemove()) {
        QTreeWidgetItem *deletionItem = new QTreeWidgetItem(deletionTree);
        deletionItem->setData(0, Qt::DisplayRole, locale.toString(rideItem->dateTime.date(), QLocale::ShortFormat));
        deletionItem->setData(1, Qt::DisplayRole, PlanBundle::getRideSport(rideItem));
        deletionItem->setData(2, Qt::DisplayRole, PlanBundle::getRideName(rideItem));
        ++deletionCount;
    }
    deletionLabel->setVisible(deletionCount > 0);
    deletionTree->setVisible(deletionCount > 0);
    targetRangeBar->setResult(
        wiz->planReader->getTargetRangeStart(),
        wiz->planReader->getTargetRangeEnd(),
        wiz->planReader->getNumSelectedActivities(),
        wiz->planReader->getActivitiesToRemove().count());
}


bool
ImportPlanPageSummary::validatePage
()
{
    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader) {
        return false;
    }

    QPointer<ImportPlanWizard> guardedWizard(wiz);
    PlanImportReaderLease<PlanBundleReader> readerLease(
        this, wiz->planReader);
    if (!readerLease.available())
        return false;

    OverrideCursorScope cursorScope;
    readerLease.reader()->importBundle();

    return readerLease.ownerAlive()
        && guardedWizard
        && guardedWizard->planReader
            == readerLease.sharedReader();
}


////////////////////////////////////////////////////////////////////////////////
// ImportPlanPageResult

ImportPlanPageResult::ImportPlanPageResult
(QWidget *parent)
: QWizardPage(parent)
{
    setTitle(tr("Import Plan - Result"));
    setSubTitle(tr("This page shows the outcome of importing the selected training plan."));

    setFinalPage(true);

    label = new QLabel();

    QHBoxLayout *layout = new QHBoxLayout();
    layout->addStretch(1);
    layout->addWidget(label);
    layout->addStretch(1);

    setLayout(layout);
}


int
ImportPlanPageResult::nextId
() const
{
    return ImportPlanWizard::Finalize;
}


void
ImportPlanPageResult::initializePage
()
{
    ImportPlanWizard *wiz = qobject_cast<ImportPlanWizard*>(wizard());
    if (wiz == nullptr || !wiz->planReader) {
        return;
    }

    QString message;
    if (wiz->planReader->getLastImportResult().ok()) {
        if (wiz->planReader->getLastImportResult()
                .warnings.isEmpty()) {
            message = HLO + tr("Import completed successfully.") + HLC + "<br>";
            message += tr("All activities and workouts were imported without issues.");
        } else {
            message = HLO + tr("Import completed with warnings.") + HLC + "<br>";
            message += tr("Some items could not be fully processed.");
        }
    } else {
        message = HLO + tr("Import failed.") + HLC + "<br>";
        message += tr("The bundle could not be fully imported.");
        message += QString("<br>") + HLO + tr("Errors") + HLC;
        message += "<ul>";
        for (const QString &error :
             wiz->planReader->getLastImportResult().errors) {
            message += "<li>" + error + "</li>";
        }
        message += "</ul>";
    }
    if (!wiz->planReader->getLastImportResult()
            .warnings.isEmpty()) {
        message += QString("<br>") + HLO + tr("Warnings") + HLC;
        message += "<ul>";
        for (const QString &warning :
             wiz->planReader->getLastImportResult().warnings) {
            message += "<li>" + warning + "</li>";
        }
        message += "</ul>";
    }
    label->setText(message);
}
