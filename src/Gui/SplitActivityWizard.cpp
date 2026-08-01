/*
 * Copyright (c) 2011 Mark Liversedge (liversedge@gmail.com)
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

#include "SplitActivityWizard.h"
#include "SplitActivitySave.h"
#include "SplitActivityWorkflow.h"
#include "SplitRideData.h"
#include "MainWindow.h"
#include "Athlete.h"
#include "Context.h"
#include "RideCache.h"
#include "SaveDialogs.h"
#include "HelpWhatsThis.h"

#include <QCoreApplication>
#include <QPointer>


// Minimum gap in recording to find a natural break to split
static const double defaultMinimumGap = 1; // 1 minute

// Minimum size of segment to identify as a new ride
static const double defaultMinimumSegmentSize = 5; // 5 minutes

namespace {

QString linkedSourceRemovalBlockingReason()
{
    return QCoreApplication::translate(
        "SplitActivityWizard",
        "This activity is linked to another activity. Remove the link or select \"Keep original\" before splitting.");
}

SplitActivitySourceIdentity splitSourceIdentity(
    const RideItem *item)
{
    return item
        ? SplitActivitySourceIdentity{
              item->fileName, item->path, item->planned}
        : SplitActivitySourceIdentity{};
}

bool splitWorkflowOwnersAreCurrent(
    const QPointer<Context> &context,
    const QPointer<Athlete> &athlete,
    const QPointer<RideCache> &cache,
    const QPointer<AthleteDirectoryStructure> &home)
{
    return context
        && athlete
        && cache
        && home
        && context->athlete == athlete.data()
        && athlete->rideCache == cache.data()
        && athlete->home == home.data();
}

bool splitWorkflowSourceIsCurrent(
    const QPointer<Context> &context,
    const QPointer<Athlete> &athlete,
    const QPointer<RideCache> &cache,
    const QPointer<AthleteDirectoryStructure> &home,
    const QPointer<RideItem> &source,
    const SplitActivitySourceIdentity &expected)
{
    const bool ownersCurrent =
        splitWorkflowOwnersAreCurrent(
            context, athlete, cache, home);
    RideItem *const item = source.data();
    const bool sourceInCache = ownersCurrent
        && item
        && cache->rides().contains(item);
    return splitActivitySourceIsCurrent(
        ownersCurrent, ownersCurrent,
        item != nullptr, sourceInCache,
        expected, splitSourceIdentity(item));
}

bool splitWorkflowSourceSnapshotIsCurrent(
    const QPointer<Context> &context,
    const QPointer<Athlete> &athlete,
    const QPointer<RideCache> &cache,
    const QPointer<AthleteDirectoryStructure> &home,
    const QPointer<RideItem> &source,
    const SplitActivitySourceIdentity &expectedIdentity,
    const SplitActivityContentSnapshot &expectedContent,
    const SplitActivityContentSnapshot &currentContent)
{
    const bool ownersCurrent =
        splitWorkflowOwnersAreCurrent(
            context, athlete, cache, home);
    RideItem *const item = source.data();
    const bool sourceInCache = ownersCurrent
        && item
        && cache->rides().contains(item);
    return splitActivitySourceSnapshotIsCurrent(
        ownersCurrent, ownersCurrent,
        item != nullptr, sourceInCache,
        expectedIdentity, splitSourceIdentity(item),
        expectedContent, currentContent);
}

} // namespace

void SplitActivityWizard::bindSourceRideContent(RideFile *ride)
{
    if (trackedSourceRide.data() == ride) return;

    for (const QMetaObject::Connection &connection :
         sourceContentConnections) {
        disconnect(connection);
    }
    sourceContentConnections.clear();
    trackedSourceRide = ride;
    ++sourceContentRevision;
    if (!ride) return;

    sourceContentConnections.append(connect(
        ride, &RideFile::modified,
        this, &SplitActivityWizard::invalidateSourceRideContent));
    sourceContentConnections.append(connect(
        ride, &RideFile::saved,
        this, &SplitActivityWizard::invalidateSourceRideContent));
    sourceContentConnections.append(connect(
        ride, &RideFile::reverted,
        this, &SplitActivityWizard::invalidateSourceRideContent));
    sourceContentConnections.append(connect(
        ride, &RideFile::deleted,
        this, &SplitActivityWizard::invalidateSourceRideContent));
    sourceContentConnections.append(connect(
        ride, &QObject::destroyed, this, [this] {
            trackedSourceRide.clear();
            sourceContentConnections.clear();
            ++sourceContentRevision;
        }));
}

void SplitActivityWizard::invalidateSourceRideContent()
{
    ++sourceContentRevision;
}

SplitActivityContentSnapshot
SplitActivityWizard::currentSourceContentSnapshot() const
{
    RideItem *const item = rideItem.data();
    RideFile *const currentRide = item
        ? item->ride()
        : nullptr;
    return {
        reinterpret_cast<quintptr>(currentRide),
        sourceContentRevision};
}

// Main wizard
SplitActivityWizard::SplitActivityWizard(Context *context)
    : QWizard(context ? context->mainWindow : nullptr),
      context(context),
      athlete(context ? context->athlete : nullptr),
      rideCache(context && context->athlete
          ? context->athlete->rideCache
          : nullptr),
      home(context && context->athlete
          ? context->athlete->home
          : nullptr)
{
#ifdef Q_OS_MAX
    setWizardStyle(QWizard::ModernStyle);
#endif

    // delete when done
    setAttribute(Qt::WA_DeleteOnClose);

    // Minimum 600x500 for when selecting intervals
    setMinimumHeight(500 *dpiXFactor);
    setMinimumWidth(600 *dpiYFactor);

    // title
    setWindowTitle(tr("Split Activity"));

    // help
    HelpWhatsThis *help = new HelpWhatsThis(this);
    this->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::MenuBar_Activity_SplitRide));

    // set ride - unconst since we will wipe it away eventually
    rideItem = context
        ? const_cast<RideItem*>(context->currentRideItem())
        : nullptr;
    sourceIdentity = splitSourceIdentity(
        rideItem.data());
    bindSourceRideContent(
        rideItem ? rideItem->ride() : nullptr);
    if (rideItem) {
        connect(
            rideItem, &RideItem::rideDataChanged,
            this, [this] {
                ++sourceContentRevision;
                bindSourceRideContent(
                    rideItem ? rideItem->ride() : nullptr);
            });
        connect(
            rideItem, &RideItem::rideMetadataChanged,
            this, &SplitActivityWizard::invalidateSourceRideContent);
    }

    // Set sensible defaults
    keepOriginal = false;
    minimumGap = defaultMinimumGap;
    minimumSegmentSize = defaultMinimumSegmentSize;
    usedMinimumSegmentSize = usedMinimumGap = -1;

    // set initial intervals list, will be adjusted
    // if the user modifies the default parameters
    intervals = new QTreeWidget;
    intervals->headerItem()->setText(0, tr(""));
    intervals->headerItem()->setText(1, tr("Start"));
    intervals->headerItem()->setText(2, tr(""));
    intervals->headerItem()->setText(3, tr("Stop"));
    intervals->headerItem()->setText(4, tr("Duration"));
    intervals->headerItem()->setText(5, tr("Distance"));
    intervals->headerItem()->setText(6, tr("Interval Name"));
    intervals->setColumnCount(7);
    intervals->setColumnWidth(0,30*dpiXFactor);
    intervals->setColumnWidth(1,80*dpiXFactor);
    intervals->setColumnWidth(2,30*dpiXFactor);
    intervals->setColumnWidth(3,80*dpiXFactor);
    intervals->setColumnWidth(4,80*dpiXFactor);
    intervals->setColumnWidth(5,80*dpiXFactor);
    intervals->setSelectionMode(QAbstractItemView::NoSelection);
    intervals->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    intervals->setUniformRowHeights(true);
    intervals->setIndentation(0);

    files = new QTreeWidget;
    files->headerItem()->setText(0, tr("Filename"));
    files->headerItem()->setText(1, tr("Date"));
    files->headerItem()->setText(2, tr("Time"));
    files->headerItem()->setText(3, tr("Duration"));
    files->headerItem()->setText(4, tr("Distance"));
    files->headerItem()->setText(5, tr("Action"));
    files->setColumnCount(6);
    files->setColumnWidth(0, 190*dpiXFactor); // filename
    files->setColumnWidth(1, 95*dpiXFactor); // date
    files->setColumnWidth(2, 90*dpiXFactor); // time
    files->setColumnWidth(3, 75*dpiXFactor); // duration
    files->setColumnWidth(4, 75*dpiXFactor); // distance
    files->setSelectionMode(QAbstractItemView::SingleSelection);
    files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    files->setUniformRowHeights(true);
    files->setIndentation(0);

    // just the hr and power as a plot
    smallPlot = new SmallPlot(this);
    smallPlot->setFixedHeight(100);
    smallPlot->setData(rideItem.data());

    bg = new SplitBackground(this);
    bg->attach(smallPlot);

    // 5 step process, although Conflict may be skipped
    addPage(new SplitWelcome(this));
    addPage(new SplitKeep(this));
    addPage(new SplitParameters(this));
    addPage(new SplitSelect(this));
    addPage(new SplitConfirm(this));

    done = false;
}

void
SplitActivityWizard::setIntervalsList(SplitSelect *selector)
{
    RideItem *const sourceItem = rideItem.data();
    RideFile *const sourceRide = sourceItem
        ? sourceItem->ride()
        : nullptr;
    if (!sourceRide
        || sourceRide->dataPoints().isEmpty()) {
        intervals->clear();
        return;
    }

    // didn't change so no need to rebuild
    if (usedMinimumGap == minimumGap && usedMinimumSegmentSize == minimumSegmentSize) return;

    // clear the table
    intervals->clear();

    // convert to seconds
    int minimumGap = this->minimumGap * 60;
    int minimumSegmentSize = this->minimumSegmentSize * 60;

    // remember the last ones we used
    usedMinimumSegmentSize = this->minimumSegmentSize;
    usedMinimumGap = this->minimumGap;

    // find segments where gap is greater than minimumGap
    // and segment size is > minimumSize. If a segment is shorter
    // than minimumSize then ignore it (i.e. treat is as part of the gap)
    QList<RideFileInterval*> segments;
    double segmentStart = 0;
    double segmentEnd = 0;

    double lastSecs = 0;
    bool first = true;

    int counter = 0;
    foreach (RideFilePoint *p, sourceRide->dataPoints()) {

        if (first == true) {

            segmentStart = segmentEnd = lastSecs = p->secs;
            first = false;

        } else {

            if ((p->secs - segmentEnd) >= minimumGap) {

                if ((segmentEnd-segmentStart) >= minimumSegmentSize) {

                    // we have a candidate
                    segments.append(new RideFileInterval(RideFileInterval::USER, segmentStart, segmentEnd,
                                    QString(tr("Activity Segment #%1")).arg(++counter), Qt::black, false));

                }
                segmentEnd = segmentStart = p->secs;

            } else {

                // keep accumulating
                segmentEnd = p->secs;
            }
        } 
        lastSecs = p->secs;
    }

    // we got to the end, is there a segment here?
    if ((segmentEnd-segmentStart) >= minimumSegmentSize) {

        // we have a candidate
        segments.append(new RideFileInterval(RideFileInterval::USER, segmentStart, segmentEnd,
                                             QString(tr("Activity Segment #%1")).arg(++counter), Qt::black, false));

    }


    // now look at the segments and add any gaps in recording
    // because these MUST be honoured (i.e. user cannot unmark
    // these points, otherwise gaps in recording are retained
    // in the resulting file
    foreach(RideFileInterval *p, gaps) { delete p; } gaps.clear(); // wip old ones

    double lastsecs = sourceRide->dataPoints().first()->secs;
    int gapnum = 0;
    foreach(RideFileInterval *ride, segments) {
        if (ride->start > lastsecs) {
            // we have a gap
            gapnum++;

            // add to gap list
            RideFileInterval *gap = new RideFileInterval(RideFileInterval::USER,
                                                         lastsecs,
                                                         ride->start,
                                                         QString(tr("Gap in recording #%1")).arg(gapnum), Qt::black, false);
            gaps.append(gap);

            // add to interval list
            segments.append(new RideFileInterval(*gap));
        }
        lastsecs = ride->stop;
    }
    if (lastsecs < sourceRide->dataPoints().last()->secs) {
        // gap at the end
        gapnum++;

        // add to gap list
        RideFileInterval *gap = new RideFileInterval(RideFileInterval::USER,
                                                     lastsecs,
                                                     sourceRide->dataPoints().last()->secs,
                                                     QString(tr("Gap in recording #%1")).arg(gapnum), Qt::black, false);
        gaps.append(gap);

        // add to interval list
        segments.append(new RideFileInterval(*gap));
    }

    // first entry in list should always be entire file
    // so we can mark the start and stop for splitting
    segments.insert(0, new RideFileInterval(RideFileInterval::USER, sourceRide->dataPoints().first()->secs,
                                     sourceRide->dataPoints().last()->secs,
                                     tr("Entire Activity"), Qt::black, false));

    // now fold in the ride intervals
    segments.append(sourceRide->intervals());

    // now lets sort the segments in start order
    std::sort(segments.begin(), segments.end());

    // first just add all the current ride intervals
    counter = 0;
    QChar zero = QLatin1Char('0');
    foreach (RideFileInterval *interval, segments) {

        // DO NOT skip intervals that are too short
        //if (interval.stop - interval.start < minimumSegmentSize) continue;

        QTreeWidgetItem *add = new QTreeWidgetItem(intervals->invisibleRootItem());
        add->setFlags(add->flags() | Qt::ItemIsEditable);

        // we set these intervals as checked by default
        bool checkit = (interval->name.startsWith(tr("Gap in recording")) ||
                        interval->name == tr("Entire Activity"));

        // disable checkbox editing (i.e. mandatory split) at gaps in recording
        // we have turned this off from user requests, may reinstate or choose
        // to fix gaps when they are left behind after split.
        bool disableit = false; /* (interval.name.startsWith("Gap in recording")); */

        // selector start
        QCheckBox *checkBox = new QCheckBox("", this);
        checkBox->setChecked(checkit);
        checkBox->setEnabled(!disableit);
        intervals->setItemWidget(add, 0, checkBox);

        connect(checkBox, SIGNAL(stateChanged(int)), selector, SLOT(refreshMarkers()));

        // interval start
        int secs = interval->start;
        add->setText(1, QString("%1:%2:%3")
                        .arg(secs/3600,2,10,zero)
                        .arg(secs%3600/60,2,10,zero)
                        .arg(secs%60,2,10,zero));

        // selector stop
        checkBox = new QCheckBox("", this);
        checkBox->setChecked(checkit);
        checkBox->setEnabled(!disableit);
        intervals->setItemWidget(add, 2, checkBox);

        connect(checkBox, SIGNAL(stateChanged(int)), selector, SLOT(refreshMarkers()));

        // interval start
        secs = interval->stop;
        add->setText(3, QString("%1:%2:%3")
                        .arg(secs/3600,2,10,zero)
                        .arg(secs%3600/60,2,10,zero)
                        .arg(secs%60,2,10,zero));

        // interval duration
        secs = interval->stop - interval->start;
        add->setText(4, QString("%1:%2:%3")
                        .arg(secs/3600,2,10,zero)
                        .arg(secs%3600/60,2,10,zero)
                        .arg(secs%60,2,10,zero));

        // interval distance
        double distance = sourceRide->timeToDistance(interval->stop) -
                          sourceRide->timeToDistance(interval->start);
        add->setText(5, QString("%1 %2")
                        .arg(distance * (GlobalContext::context()->useMetricUnits ? 1 : MILES_PER_KM), 0, 'f', 2)
                        .arg(GlobalContext::context()->useMetricUnits ? "km" : "mi"));

        // interval name
        add->setText(6, interval->name);

        // hiddden columns with dataPoint from/to
        add->setText(7, QString("%1").arg(sourceRide->timeIndex(interval->start)));
        add->setText(8, QString("%1").arg(sourceRide->timeIndex(interval->stop)));

        counter++;
    }
    smallPlot->replot();
}

void
SplitActivityWizard::setFilesList()
{
    // clear the table
    files->clear();

    // fold in current ride -- if we are removing
    if (keepOriginal == false) {
        RideItem *const sourceItem = rideItem.data();
        RideFile *const sourceRide = sourceItem
            ? sourceItem->ride()
            : nullptr;
        if (!sourceRide) return;

        // we will wipe the original file
        QTreeWidgetItem *add = new QTreeWidgetItem(files->invisibleRootItem());

        add->setText(0, sourceItem->fileName);
        add->setText(1, sourceRide->startTime().toString(tr("dd MMM yyyy")));
        add->setText(2, sourceRide->startTime().toString("hh:mm:ss"));

        // get duration and distance, yuk, dup of code below, and from RideImportWizard
        int secs=0;
        double km=0;
        if (!sourceRide->dataPoints().isEmpty() && sourceRide->dataPoints().last() != NULL) {
            if (!secs) secs = sourceRide->dataPoints().last()->secs;
            if (!km) km = sourceRide->dataPoints().last()->km;
        }

        // set duration
        QChar zero = QLatin1Char ( '0' );
        QString time = QString("%1:%2:%3").arg(secs/3600,2,10,zero)
            .arg(secs%3600/60,2,10,zero)
            .arg(secs%60,2,10,zero);
        add->setText(3, time);

        // set distance
        QString dist = GlobalContext::context()->useMetricUnits
            ? QString ("%1 km").arg(km, 0, 'f', 1)
            : QString ("%1 mi").arg(km * MILES_PER_KM, 0, 'f', 1);
        add->setText(4, dist);

        // interval action
        add->setText(5, tr("Remove"));
    }

    // create a row for each file and action
    QChar zero = QLatin1Char('0');
    foreach (RideFile *ride, activities) {

        QTreeWidgetItem *add = new QTreeWidgetItem(files->invisibleRootItem());

        QString filename = QString ("%1_%2_%3_%4_%5_%6.json")
                           .arg(ride->startTime().date().year(), 4, 10, zero)
                           .arg(ride->startTime().date().month(), 2, 10, zero)
                           .arg(ride->startTime().date().day(), 2, 10, zero)
                           .arg(ride->startTime().time().hour(), 2, 10, zero)
                           .arg(ride->startTime().time().minute(), 2, 10, zero)
                           .arg(ride->startTime().time().second(), 2, 10, zero);

        // filename
        add->setText(0, filename);

        // date and time
        add->setText(1, ride->startTime().toString(tr("dd MMM yyyy")));
        add->setText(2, ride->startTime().toString("hh:mm:ss"));

        // get duration and distance
        int secs=0;
        double km=0;
        if (!ride->dataPoints().isEmpty() && ride->dataPoints().last() != NULL) {
            if (!secs) secs = ride->dataPoints().last()->secs;
            if (!km) km = ride->dataPoints().last()->km;
        }

        // set duration
        QChar zero = QLatin1Char ( '0' );
        QString time = QString("%1:%2:%3").arg(secs/3600,2,10,zero)
            .arg(secs%3600/60,2,10,zero)
            .arg(secs%60,2,10,zero);
        add->setText(3, time);

        // set distance
        QString dist = GlobalContext::context()->useMetricUnits
            ? QString ("%1 km").arg(km, 0, 'f', 1)
            : QString ("%1 mi").arg(km * MILES_PER_KM, 0, 'f', 1);
        add->setText(4, dist);

        // interval action
        add->setText(5, tr("Create"));
    }

    // tidy up column widths
    files->resizeColumnToContents(5);
}

// if the file in question has a backup file already then
// return the filename (without the path), otherwise return
// an empty string
QString
SplitActivityWizard::hasBackup(QString filename)
{
    if (!splitWorkflowOwnersAreCurrent(
            context, athlete, rideCache, home)) {
        return QString();
    }
    QString backupFilename = home->fileBackup().canonicalPath() + "/" + filename + ".bak";

    if (QFile(backupFilename).exists()) {

        return QString(filename + ".bak");

    } else {

        return "";
    }
}

QStringList
SplitActivityWizard::conflicts(QDateTime datetime)
{
    QStringList returning;
    if (!splitWorkflowOwnersAreCurrent(
            context, athlete, rideCache, home)) {
        return returning;
    }

    // Check if an existing ride has the same starttime
    QChar zero = QLatin1Char('0');
    QString targetnosuffix = QString ("%1_%2_%3_%4_%5_%6")
                           .arg(datetime.date().year(), 4, 10, zero)
                           .arg(datetime.date().month(), 2, 10, zero)
                           .arg(datetime.date().day(), 2, 10, zero)
                           .arg(datetime.time().hour(), 2, 10, zero)
                           .arg(datetime.time().minute(), 2, 10, zero)
                           .arg(datetime.time().second(), 2, 10, zero);

    // now make a regexp for all know ride types
    foreach(QString suffix, RideFileFactory::instance().suffixes()) {

        QString conflict = home->activities().canonicalPath() + "/" + targetnosuffix + "." + suffix;
        if (QFile(conflict).exists()) returning << conflict;
    }
    return returning;
}

/*----------------------------------------------------------------------
 * Wizard Pages
 *--------------------------------------------------------------------*/

// welcome
SplitWelcome::SplitWelcome(SplitActivityWizard *parent) : QWizardPage(parent), wizard(parent)
{
    setTitle(tr("Split Activity"));
    setSubTitle(tr("Lets get started"));

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    QLabel *label = new QLabel(tr("This wizard will help you split the current activity "
                               "into multiple activities\n\n"
                               "The wizard will identify segments of uninterrupted "
                               "activity and allow you to select which ones to "
                               "save as new activities. You will also be able to "
                               "select any currently defined intervals too.\n\n"
                               "If the newly created activity clashes with an existing "
                               "activity (same date and time) then the wizard will adjust "
                               "the start time by one or more seconds to avoid losing or "
                               "overwriting any existing data."));
    label->setWordWrap(true);

    layout->addWidget(label);
    layout->addStretch();
}

// Keep original?
SplitKeep::SplitKeep(SplitActivityWizard *parent) : QWizardPage(parent), wizard(parent)
{
    setTitle(tr("Keep original"));
    setSubTitle(tr("Do you want to keep the original?"));

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    QLabel *label = new QLabel(tr("If you want to keep the current activity then you "
                               "should ensure you have clicked on the \"Keep original "
                               "\" check box below.\n\n"
                               "If you do not choose to keep the original "
                               "it will be backed up before removing it from the "
                               "history.\n\n"));
    label->setWordWrap(true);

    keepOriginal = new QCheckBox(tr("Keep original"), this);
    keepOriginal->setChecked(wizard->keepOriginal);

    warning = new QLabel(this);
    warning->setWordWrap(true);
    QFont font;
    font.setWeight(QFont::Bold);
    warning->setFont(font);

    setWarning();

    layout->addWidget(label);
    layout->addWidget(keepOriginal);
    layout->addStretch();
    layout->addWidget(warning);

    connect(keepOriginal, SIGNAL(stateChanged(int)), this, SLOT(keepOriginalChanged()));
}

// parameters
SplitParameters::SplitParameters(SplitActivityWizard *parent) : QWizardPage(parent), wizard(parent)
{
    setTitle(tr("Split Parameters"));
    setSubTitle(tr("Configure how segments are found"));

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    QLabel *label = new QLabel(tr("This wizard will find segments of the activity to save "
                               "by looking for gaps in recording. \n\n"
                               "You can define the minimum length, in time, a gap "
                               "in recording should be in order to mark the end of "
                               "one segment and the beginning of another.\n\n"
                               "In addition, you can set a minimum segment size. "
                               "Any segment smaller than this limit will be ignored.\n\n"));
    label->setWordWrap(true);

    layout->addWidget(label);

    QGridLayout *grid = new QGridLayout;
    QLabel *minGap = new QLabel(tr("Minimum Gap (minutes)"), this);
    QLabel *minSize = new QLabel(tr("Minimum Segment Size (minutes)"), this);

    minimumGap = new QDoubleSpinBox(this);
    minimumGap->setDecimals(0);
    minimumGap->setSingleStep(1.0);
    minimumGap->setValue(wizard->minimumGap);

    minimumSegmentSize = new QDoubleSpinBox(this);
    minimumSegmentSize->setDecimals(0);
    minimumSegmentSize->setSingleStep(1.0);
    minimumSegmentSize->setValue(wizard->minimumSegmentSize);

    grid->addWidget(minGap,0,0);
    grid->addWidget(minimumGap,0,1,Qt::AlignLeft);
    grid->addWidget(minSize,1,0);
    grid->addWidget(minimumSegmentSize,1,1,Qt::AlignLeft);

    layout->addLayout(grid);
    layout->addStretch();

    connect (minimumGap, SIGNAL(valueChanged(double)), this, SLOT(valueChanged()));
    connect (minimumSegmentSize, SIGNAL(valueChanged(double)), this, SLOT(valueChanged()));
}

void
SplitParameters::valueChanged()
{
    wizard->minimumGap = minimumGap->value();
    wizard->minimumSegmentSize = minimumSegmentSize->value();
}

void
SplitKeep::keepOriginalChanged()
{
    wizard->keepOriginal = keepOriginal->isChecked();
    setWarning();
}

bool
SplitKeep::validatePage()
{
    QPointer<SplitKeep> page(this);
    QPointer<SplitActivityWizard> guardedWizard(
        wizard);
    if (!guardedWizard) return false;

    const QPointer<Context> guardedContext(
        guardedWizard->context);
    const QPointer<Athlete> guardedAthlete(
        guardedWizard->athlete);
    const QPointer<RideCache> guardedCache(
        guardedWizard->rideCache);
    const QPointer<AthleteDirectoryStructure> guardedHome(
        guardedWizard->home);
    QPointer<RideItem> source(
        guardedWizard->rideItem.data());
    const SplitActivitySourceIdentity expectedSource =
        guardedWizard->sourceIdentity;
    const auto sourceIsCurrent = [&] {
        return splitWorkflowSourceIsCurrent(
            guardedContext, guardedAthlete,
            guardedCache, guardedHome,
            source, expectedSource);
    };
    if (!splitActivitySourceRemovalAllowed(
            guardedWizard->keepOriginal,
            source && source->hasLinkedActivity())) {
        QMessageBox::warning(
            page.data(), tr("Split Activity"),
            linkedSourceRemovalBlockingReason());
        return false;
    }
    return prepareSplitSourceBeforeSelection(
        guardedWizard->keepOriginal,
        sourceIsCurrent,
        [&] {
            if (!page || !guardedWizard
                || !sourceIsCurrent()) {
                QMessageBox::warning(
                    page.data(), tr("Split Activity"),
                    tr("The source activity is no longer available."));
                return false;
            }
            const RideCache::OperationPreCheck check =
                guardedCache->checkRemovalLinks(
                    source.data());
            if (!page || !guardedWizard
                || !sourceIsCurrent()) {
                return false;
            }
            if (!check.canProceed) {
                QMessageBox::warning(
                    page.data(), tr("Split Activity"),
                    check.blockingReason);
                return false;
            }
            if (!proceedDialog(
                    guardedContext.data(), check)
                || !page || !guardedWizard
                || !sourceIsCurrent()) {
                return false;
            }
            const RideCache::OperationPreCheck recheck =
                guardedCache->checkRemovalLinks(
                    source.data());
            if (!page || !guardedWizard
                || !sourceIsCurrent()) {
                return false;
            }
            if (!recheck.canProceed
                || recheck.requiresUserDecision) {
                QMessageBox::warning(
                    page.data(), tr("Split Activity"),
                    recheck.blockingReason.isEmpty()
                        ? tr("The source activity changed while preparing the split.")
                        : recheck.blockingReason);
                return false;
            }
            return true;
        },
        [&] {
            if (!page || !guardedWizard
                || !sourceIsCurrent()) {
                return;
            }
            guardedWizard->rideItem = source.data();
            guardedWizard->usedMinimumGap = -1;
            guardedWizard->usedMinimumSegmentSize = -1;
            guardedWizard->smallPlot->setData(
                source.data());
        });
}

void
SplitKeep::setWarning()
{
    if (!keepOriginal->isChecked()) {
        RideItem *const source =
            wizard->rideItem.data();
        if (!source) {
            warning->setText(
                tr("The source activity is no longer available."));
            return;
        }

        if (!splitActivitySourceRemovalAllowed(
                false, source->hasLinkedActivity())) {
            warning->setText(
                linkedSourceRemovalBlockingReason());
            return;
        }

        if (wizard->hasBackup(source->fileName) != "") {

            warning->setText(tr("WARNING: The current activity will be backed up and "
                             "removed, but a backup already exists. The existing "
                             "backup will therefore be overwritten."));
            return;
        }
    } 
    warning->setText("");
}

// Select
SplitSelect::SplitSelect(SplitActivityWizard *parent) : QWizardPage(parent), wizard(parent)
{
    setTitle(tr("Select Split Markers"));
    setSubTitle(tr("Activity will be split between marker points selected"));

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    layout->addWidget(wizard->smallPlot);
    layout->addWidget(wizard->intervals);
}
void
SplitSelect::initializePage()
{
    if (!wizard->rideItem) {
        wizard->intervals->clear();
        refreshMarkers();
        return;
    }
    wizard->setIntervalsList(this);
    refreshMarkers();
}

void
SplitSelect::refreshMarkers()
{
    // update the markers on the plot
    foreach (QwtPlotMarker *m, wizard->markers) {
        // remove from the plot and delete
        m->detach();
        delete m;
    }
    wizard->markers.clear();
    wizard->marks.clear(); // dataPoint indexes

    RideItem *const sourceItem =
        wizard->rideItem.data();
    RideFile *const sourceRide = sourceItem
        ? sourceItem->ride()
        : nullptr;
    if (!sourceRide) {
        wizard->smallPlot->replot();
        return;
    }
    const auto &dataPoints =
        sourceRide->dataPoints();

    // now refresh them
    for(int i=0; i<wizard->intervals->invisibleRootItem()->childCount(); i++) {

        QTreeWidgetItem *current = wizard->intervals->invisibleRootItem()->child(i);

        // add marker for start?
        if (static_cast<QCheckBox*>(wizard->intervals->itemWidget(current,0))->isChecked()) {

            long index = current->text(7).toInt();
            if (index < 0 || index >= dataPoints.size())
                continue;
            double point = dataPoints.at(index)->secs;

            wizard->marks.append(index);
            QwtPlotMarker *add = new QwtPlotMarker;
            wizard->markers.append(add);

            // vertical line will do for now
            add->setLineStyle(QwtPlotMarker::VLine);
            add->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
            add->setLinePen(QPen(GColor(CPLOTMARKER), 0, Qt::DashDotLine));
            add->setValue(point / 60.0, 0.0);
            add->attach(wizard->smallPlot);
        }

        // add marker for stop?
        if (static_cast<QCheckBox*>(wizard->intervals->itemWidget(current,2))->isChecked()) {

            long index = current->text(8).toInt();
            if (index < 0 || index >= dataPoints.size())
                continue;
            double point = dataPoints.at(index)->secs;

            wizard->marks.append(index);
            QwtPlotMarker *add = new QwtPlotMarker;
            wizard->markers.append(add);

            // vertical line will do for now
            add->setLineStyle(QwtPlotMarker::VLine);
            add->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
            add->setLinePen(QPen(GColor(CPLOTMARKER), 0, Qt::DashDotLine));
            add->setValue(point / 60.0, 0.0);
            add->attach(wizard->smallPlot);
        }
    }
    wizard->smallPlot->replot();
}

// Confirm
SplitConfirm::SplitConfirm(SplitActivityWizard *parent) : QWizardPage(parent), wizard(parent)
{
    setTitle(tr("Confirm"));
    setSubTitle(tr("Split activity cannot be undone"));

    setCommitPage(true);
    setButtonText(QWizard::CommitButton, tr("Confirm"));

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    layout->addWidget(wizard->files);
}

// create an array of rides
void
SplitConfirm::initializePage()
{
    // clear the current array
    foreach(RideFile *ride, wizard->activities) {
        delete ride;
    }
    wizard->activities.clear();
    wizard->sourceContentSnapshot = {};

    RideItem *const sourceItem =
        wizard->rideItem.data();
    RideFile *const sourceRide = sourceItem
        ? sourceItem->ride()
        : nullptr;
    if (!sourceRide) {
        wizard->setFilesList();
        return;
    }
    const auto &dataPoints =
        sourceRide->dataPoints();

    // create a sorted list of markers, since we
    // may have duplicates and the sequence is
    // not guaranteed to be ordered
    QList<long> points;
    foreach(long mark, wizard->marks) points.append(mark); // marks are indexes to ensure absolute accuracy
    std::sort(points.begin(), points.end());

    // Create a new ride for each marked segment
    long lastmark = -1;
    foreach(long mark, points) {

        if (mark == lastmark) continue;

        if (lastmark != -1) {
            if (lastmark < 0
                || lastmark >= dataPoints.size()
                || mark < 0
                || mark >= dataPoints.size()) {
                lastmark = mark;
                continue;
            }

            // ignore gaps!
            if (!wizard->bg->isGap(dataPoints.at(lastmark)->secs/60.0,
                                   dataPoints.at(mark)->secs/60.0)) {

                const SplitSegmentEnd segmentEnd =
                    mark == points.constLast()
                        ? SplitSegmentEnd::Include
                        : SplitSegmentEnd::Exclude;
                RideFile *add = extractSplitRideSegment(
                    *sourceRide, lastmark, mark, segmentEnd);
                wizard->activities.append(add);
            }
        }
        lastmark = mark;
    }

    // Adjust start times so the source remains reserved until commit.
    // Record any adjustment in metadata for transparency.
    QList<QDateTime> toBeCreated;
    foreach(RideFile *ride, wizard->activities) {

        int adjust = 0;
        QStringList conflicts = wizard->conflicts(ride->startTime());

        while (conflicts.count() || toBeCreated.contains(ride->startTime())) {

            adjust++;
            ride->setStartTime(ride->startTime().addSecs(1));
            conflicts = wizard->conflicts(ride->startTime());
        }

        // record the fact we adjusted...
        if (adjust) {
            ride->setTag("Start Time Adjust", QString("%1 seconds").arg(adjust));
        }
        toBeCreated.append(ride->startTime());
    }

    wizard->setFilesList();
    wizard->sourceContentSnapshot =
        wizard->currentSourceContentSnapshot();
}


bool
SplitConfirm::validatePage()
{
    QPointer<SplitConfirm> page(this);
    QPointer<SplitActivityWizard> guardedWizard(
        wizard);
    if (!guardedWizard) return false;

    const QPointer<Context> guardedContext(
        guardedWizard->context);
    const QPointer<Athlete> guardedAthlete(
        guardedWizard->athlete);
    const QPointer<RideCache> guardedCache(
        guardedWizard->rideCache);
    const QPointer<AthleteDirectoryStructure> guardedHome(
        guardedWizard->home);
    QPointer<RideItem> source(
        guardedWizard->rideItem.data());
    const SplitActivitySourceIdentity expectedSource =
        guardedWizard->sourceIdentity;
    const SplitActivityContentSnapshot expectedContent =
        guardedWizard->sourceContentSnapshot;
    const bool keepOriginal =
        guardedWizard->keepOriginal;

    const auto ownersAreCurrent = [&] {
        return splitWorkflowOwnersAreCurrent(
            guardedContext, guardedAthlete,
            guardedCache, guardedHome);
    };
    const auto sourceIsCurrent = [&] {
        return guardedWizard
            && splitWorkflowSourceSnapshotIsCurrent(
            guardedContext, guardedAthlete,
            guardedCache, guardedHome,
            source, expectedSource, expectedContent,
            guardedWizard->currentSourceContentSnapshot());
    };
    const auto setSourceStatus =
        [guardedWizard](const QString &status) {
            if (!guardedWizard
                || !guardedWizard->files) {
                return;
            }
            QTreeWidgetItem *const row =
                guardedWizard->files
                    ->invisibleRootItem()->child(0);
            if (row) row->setText(5, status);
        };
    const auto finishWithRecovery =
        [&](const QString &message) {
            if (guardedWizard) {
                if (!keepOriginal) {
                    setSourceStatus(
                        tr("Recovery required"));
                }
                guardedWizard->done = true;
            }
            if (page) {
                page->setTitle(
                    tr("Recovery Required"));
                page->setSubTitle(
                    tr("Split files saved; activity cleanup requires attention"));
                QMessageBox::critical(
                    page.data(), tr("Split Activity"),
                    message);
            }
            return true;
        };

    if (guardedWizard->activities.isEmpty()) {
        QMessageBox::warning(
            page.data(), tr("Split Activity"),
            tr("Select at least two split markers before continuing."));
        return false;
    }
    if (expectedSource.fileName.isEmpty()
        || expectedSource.path.isEmpty()) {
        QMessageBox::critical(
            page.data(), tr("Split Activity"),
            tr("The source activity is no longer available."));
        return false;
    }
    if (!sourceIsCurrent()) {
        QMessageBox::critical(
            page.data(), tr("Split Activity"),
            tr("The source activity changed before the split was confirmed."));
        return false;
    }
    if (!splitActivitySourceRemovalAllowed(
            keepOriginal,
            source && source->hasLinkedActivity())) {
        QMessageBox::warning(
            page.data(), tr("Split Activity"),
            linkedSourceRemovalBlockingReason());
        return false;
    }

    if (QMessageBox::question(
            page.data(), tr("Confirm"),
            tr("%1 file(s) will be created.\n\n"
               "Are you sure you wish to proceed?")
                .arg(guardedWizard->activities.count()),
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Ok) {
        return false;
    }

    if (!page || !guardedWizard) return false;
    if (!sourceIsCurrent()) {
        QMessageBox::critical(
            page.data(), tr("Split Activity"),
            tr("The source activity changed while confirming the split."));
        return false;
    }
    if (!splitActivitySourceRemovalAllowed(
            keepOriginal,
            source && source->hasLinkedActivity())) {
        QMessageBox::warning(
            page.data(), tr("Split Activity"),
            linkedSourceRemovalBlockingReason());
        return false;
    }

    if (!keepOriginal) {
        const RideCache::OperationPreCheck check =
            guardedCache->checkRemovalLinks(
                source.data());
        if (!page || !guardedWizard
            || !sourceIsCurrent()) {
            return false;
        }
        if (!check.canProceed
            || check.requiresUserDecision) {
            QMessageBox::warning(
                page.data(), tr("Split Activity"),
                check.blockingReason.isEmpty()
                    ? tr("The source activity changed while preparing the split.")
                    : check.blockingReason);
            return false;
        }
    }

    const QDir activitiesDirectory =
        guardedHome->activities();
    const QString sourcePath =
        QDir(expectedSource.path).filePath(
            expectedSource.fileName);
    const QString backupPath =
        guardedHome->fileBackup().filePath(
            expectedSource.fileName
                + QStringLiteral(".bak"));

    QList<SplitActivityOutput> outputs;
    const int offset = keepOriginal ? 0 : 1;
    for (int index = 0;
         index < guardedWizard->activities.count(); ++index) {
        QTreeWidgetItem *row =
            guardedWizard->files
                ->invisibleRootItem()->child(index + offset);
        if (!row) {
            QMessageBox::critical(
                page.data(), tr("Split Activity"),
                tr("The split activity file list is incomplete."));
            return false;
        }

        RideFile *ride =
            guardedWizard->activities.at(index);
        SplitActivityOutput output;
        output.fileName = row->text(0);
        output.stage =
            [guardedWizard, guardedContext,
             guardedAthlete, guardedCache,
             guardedHome, source, expectedSource,
             expectedContent, ride](
                const QString &stagingPath, QString &stageError) {
                if (!guardedWizard
                    || !splitWorkflowSourceSnapshotIsCurrent(
                        guardedContext, guardedAthlete,
                        guardedCache, guardedHome,
                        source, expectedSource, expectedContent,
                        guardedWizard->currentSourceContentSnapshot())) {
                    stageError = QObject::tr(
                        "The source activity changed before the split could be saved");
                    return false;
                }
                JsonFileReader reader;
                QFile stagingFile(stagingPath);
                const bool written = reader.writeRideFile(
                    guardedContext.data(), ride, stagingFile,
                    stageError, false);
                if (!guardedWizard
                    || !splitWorkflowSourceSnapshotIsCurrent(
                        guardedContext, guardedAthlete,
                        guardedCache, guardedHome,
                        source, expectedSource, expectedContent,
                        guardedWizard->currentSourceContentSnapshot())) {
                    if (stageError.isEmpty()) {
                        stageError = QObject::tr(
                            "The activity collection changed while saving the split");
                    }
                    return false;
                }
                return written;
            };
        outputs.append(output);
    }

    if (!sourceIsCurrent()) return false;
    if (!splitActivitySourceRemovalAllowed(
            keepOriginal,
            source && source->hasLinkedActivity())) {
        QMessageBox::warning(
            page.data(), tr("Split Activity"),
            linkedSourceRemovalBlockingReason());
        return false;
    }

    QStringList publishedFileNames;
    QString error;
    if (!saveSplitActivityFiles(
            activitiesDirectory, sourcePath, backupPath,
            outputs, keepOriginal,
            publishedFileNames, error)) {
        if (page) {
            QMessageBox::critical(
                page.data(), tr("Split Activity"),
                tr("The activity could not be split.\n\n%1")
                    .arg(error));
        }
        return false;
    }

    if (!page || !guardedWizard) return true;
    if (!sourceIsCurrent()) {
        return finishWithRecovery(
            tr("The split files were saved, but the original activity or its activity collection changed before cleanup. Do not retry the split."));
    }

    if (!error.isEmpty()) {
        QMessageBox::warning(
            page.data(), tr("Split Activity"),
            tr("The files were saved with a recovery warning.\n\n%1")
                .arg(error));
        if (!page || !guardedWizard) return true;
        if (!sourceIsCurrent()) {
            return finishWithRecovery(
                tr("The split files were saved, but the original activity or its activity collection changed while recovery information was shown. Do not retry the split."));
        }
    }

    bool removalNeedsAttention = false;
    if (!keepOriginal) {
        if (!sourceIsCurrent()) {
            return finishWithRecovery(
                tr("The split files were saved, but the original activity changed before the activity list could be updated. Do not retry the split."));
        }

        const RideCache::RemovalResult removal =
            guardedCache->removeArchivedRideResult(
                source.data());
        if (!page || !guardedWizard) return true;
        if (!ownersAreCurrent()) {
            return finishWithRecovery(
                tr("The split files were saved, but the activity collection changed during cleanup. Do not retry the split."));
        }

        if (removal.cleanlyCompleted()) {
            setSourceStatus(tr("Removed"));
        } else if (removal.allLogicallyRemoved()) {
            setSourceStatus(
                tr("Removed with warning"));
            QMessageBox::warning(
                page.data(), tr("Split Activity"),
                tr("The files were saved and the original activity "
                   "was removed, but cleanup requires attention.\n\n%1")
                    .arg(removal.error));
        } else {
            removalNeedsAttention = true;
            setSourceStatus(
                tr("Recovery required"));
            QMessageBox::critical(
                page.data(), tr("Split Activity"),
                tr("The split files were saved and the original was archived, "
                   "but activity cleanup did not finish. Do not retry the split. "
                   "The activity list or linked metadata may require manual "
                   "recovery.\n\n%1")
                    .arg(removal.error));
        }

        if (!page || !guardedWizard) return true;
        if (!ownersAreCurrent()) {
            return finishWithRecovery(
                tr("The split files were saved, but the activity collection changed while cleanup information was shown. Do not retry the split."));
        }
    }

    for (int index = 0;
         index < publishedFileNames.count(); ++index) {
        if (!ownersAreCurrent()
            || (keepOriginal
                && !sourceIsCurrent())) {
            return finishWithRecovery(
                tr("The split files were saved, but the activity collection changed before all new activities could be loaded. Do not retry the split."));
        }
        guardedAthlete->addRide(
            publishedFileNames.at(index), true);
        if (!page || !guardedWizard) return true;
        if (!ownersAreCurrent()
            || (keepOriginal
                && !sourceIsCurrent())) {
            return finishWithRecovery(
                tr("The split files were saved, but the activity collection changed while loading the new activities. Do not retry the split."));
        }
        QTreeWidgetItem *row =
            guardedWizard->files
                ->invisibleRootItem()->child(index + offset);
        if (row) row->setText(5, tr("Saved"));
    }

    if (!page || !guardedWizard) return true;
    page->setTitle(removalNeedsAttention
        ? tr("Recovery Required")
        : tr("Completed"));
    page->setSubTitle(removalNeedsAttention
        ? tr("Split files saved; activity cleanup requires attention")
        : tr("Split Activity Completed"));
    guardedWizard->done = true;
    return true;
}

bool
SplitConfirm::isComplete() const
{
    return !wizard->done;
}
