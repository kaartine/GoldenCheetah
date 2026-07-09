/*
 * Copyright (c) 2017 Joern Rischmueller (joern.rm@gmail.com)
 * Copyright (c) 2017 Ale Martinez (amtriathlon@gmail.com)
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

#include "MeasuresDownload.h"
#include "Measures.h"
#include "Athlete.h"
#include "RideCache.h"
#include "HelpWhatsThis.h"
#include "CloudService.h"

#include <QList>
#include <QPointer>
#include <QMutableListIterator>
#include <QGroupBox>
#include <QMessageBox>

#include <utility>


#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
namespace {
MeasuresDownload::AutoDownloadProbe autoDownloadProbe;
MeasuresDownload::ManualDownloadProbe manualDownloadProbe;
}

void MeasuresDownload::setAutoDownloadProbeForTest(
        AutoDownloadProbe probe)
{
    autoDownloadProbe = std::move(probe);
}

void MeasuresDownload::setManualDownloadProbeForTest(
        ManualDownloadProbe probe)
{
    manualDownloadProbe = std::move(probe);
}
#endif

MeasuresDownload::MeasuresDownload(
        Context *context, MeasuresGroup *measuresGroup)
    : context(context), measuresGroup(measuresGroup)
{
    if (!this->context
        || !this->context->athlete
        || !measuresGroup) {
        return;
    }

    measuresGroupSymbol = measuresGroup->getSymbol();
    measuresGroupName = measuresGroup->getName();
    connect(
        this->context, &QObject::destroyed,
        this, &QDialog::reject);
    connect(
        this->context->athlete, &QObject::destroyed,
        this, &QDialog::reject);

    setWindowTitle(tr("%1 Measures download").arg(measuresGroupName));

    HelpWhatsThis *help = new HelpWhatsThis(this);
    this->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::MenuBar_Tools_Download_Measures));

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QGroupBox *groupBox1 = new QGroupBox(tr("Choose the download or import source"));
    downloadWithings = new QRadioButton(tr("Withings"));
    downloadTredict = new QRadioButton(tr("Tredict"));
    downloadCSV = new QRadioButton(tr("Import CSV file"));
    QVBoxLayout *vbox1 = new QVBoxLayout;
    vbox1->addWidget(downloadWithings);
    vbox1->addWidget(downloadTredict);
    vbox1->addWidget(downloadCSV);
    groupBox1->setLayout(vbox1);
    mainLayout->addWidget(groupBox1);

    QGroupBox *groupBox2 = new QGroupBox(tr("Choose date range for download"));
    dateRangeAll = new QRadioButton(tr("From date of first recorded activity to today"));
    dateRangeLastMeasure = new QRadioButton(tr("From date of last downloaded measurement to today"));
    dateRangeManual = new QRadioButton(tr("Enter manually:"));
    dateRangeAll->setChecked(true);
    QVBoxLayout *vbox2 = new QVBoxLayout;
    vbox2->addWidget(dateRangeAll);
    vbox2->addWidget(dateRangeLastMeasure);
    vbox2->addWidget(dateRangeManual);
    vbox2->addStretch();
    QLabel *fromLabel = new QLabel("From");
    QLabel *toLabel = new QLabel("To");
    manualFromDate = new QDateEdit(this);
    manualFromDate->setDate(QDate::currentDate().addMonths(-1));
    manualToDate = new QDateEdit(this);
    manualToDate->setDate(QDate::currentDate());
    manualFromDate->setCalendarPopup(true);
    manualToDate->setCalendarPopup(true);
    manualFromDate->setEnabled(false);
    manualToDate->setEnabled(false);
    QHBoxLayout *dateRangeLayout = new QHBoxLayout;
    dateRangeLayout->addStretch();
    dateRangeLayout->addWidget(fromLabel);
    dateRangeLayout->addWidget(manualFromDate);
    dateRangeLayout->addWidget(toLabel);
    dateRangeLayout->addWidget(manualToDate);
    vbox2->addLayout(dateRangeLayout);
    groupBox2->setLayout(vbox2);
    mainLayout->addWidget(groupBox2);

    discardExistingMeasures = new QCheckBox(tr("Discard all existing measures"), this);
    discardExistingMeasures->setChecked(false);
    mainLayout->addWidget(discardExistingMeasures);

    progressBar = new QProgressBar(this);
    progressBar->setMinimum(0);
    progressBar->setMaximum(1);
    progressBar->setValue(0);
    QHBoxLayout *progressLayout = new QHBoxLayout;
    progressLayout->addWidget(progressBar);
    mainLayout->addLayout(progressLayout);

    downloadButton = new QPushButton(tr("Download"));
    closeButton = new QPushButton(tr("Close"));
    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(downloadButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(closeButton);
    mainLayout->addLayout(buttonLayout);


    connect(downloadButton, SIGNAL(clicked()), this, SLOT(download()));
    connect(closeButton, SIGNAL(clicked()), this, SLOT(close()));
    connect(dateRangeAll, SIGNAL(toggled(bool)), this, SLOT(dateRangeAllSettingChanged(bool)));
    connect(dateRangeLastMeasure, SIGNAL(toggled(bool)), this, SLOT(dateRangeLastSettingChanged(bool)));
    connect(dateRangeManual, SIGNAL(toggled(bool)), this, SLOT(dateRangeManualSettingChanged(bool)));

    connect(downloadWithings, SIGNAL(toggled(bool)), this, SLOT(downloadWithingsSettingChanged(bool)));
    connect(downloadTredict, SIGNAL(toggled(bool)), this, SLOT(downloadTredictSettingChanged(bool)));
    connect(downloadCSV, SIGNAL(toggled(bool)), this, SLOT(downloadCSVSettingChanged(bool)));

    // don't allow options which are not authorized
    downloadWithings->setEnabled((measuresGroup->getSymbol() == "Body") &&
        !appsettings->cvalue(context->athlete->cyclist, GC_NOKIA_REFRESH_TOKEN, "").toString().isEmpty());

    downloadTredict->setEnabled(
        (measuresGroup->getSymbol() == "Body" || measuresGroup->getSymbol() == "Hrv") &&
        !appsettings->cvalue(context->athlete->cyclist, GC_TREDICT_REFRESH_TOKEN, "").toString().isEmpty());

    // select the default checked / based on available properties and last selection
    int last_selection = appsettings->cvalue(context->athlete->cyclist, GC_BM_LAST_TYPE, 0).toInt();
    bool done = false;
    if (downloadWithings->isEnabled()) {
        if (last_selection == 0 || last_selection == WITHINGS) {
            downloadWithings->setChecked(true);
            done = true;
        }
    }
    if (!done && downloadTredict->isEnabled()) {
        if (last_selection == 0 || last_selection == TREDICT) {
            downloadTredict->setChecked(true);
            done = true;
        }
    }
    if (!done) {
        downloadCSV->setChecked(true);
    }

    // set the default from "last"
    int last_timeframe = appsettings->cvalue(context->athlete->cyclist, GC_BM_LAST_TIMEFRAME, ALL).toInt();
    switch (last_timeframe) {
    case ALL:
        dateRangeAll->setChecked(true);
        break;
    case LAST:
        dateRangeLastMeasure->setChecked(true);
        break;
    case MANUAL:
        dateRangeManual->setChecked(true);
        manualFromDate->setEnabled(true);
        manualToDate->setEnabled(true);
        break;
    default:
        dateRangeAll->setChecked(true);
    }

    // initialize the downloaders
    withingsDownload = new WithingsDownload(context);
    tredictDownload = new TredictMeasuresDownload(context);
    csvFileImport = new MeasuresCsvImport(context, this);

    // connect the progress bar
    connect(withingsDownload, SIGNAL(downloadStarted(int)), this, SLOT(downloadProgressStart(int)));
    connect(withingsDownload, SIGNAL(downloadProgress(int)), this, SLOT(downloadProgress(int)));
    connect(withingsDownload, SIGNAL(downloadEnded(int)), this, SLOT(downloadProgressEnd(int)));

    connect(tredictDownload, SIGNAL(downloadStarted(int)), this, SLOT(downloadProgressStart(int)));
    connect(tredictDownload, SIGNAL(downloadProgress(int)), this, SLOT(downloadProgress(int)));
    connect(tredictDownload, SIGNAL(downloadEnded(int)), this, SLOT(downloadProgressEnd(int)));

    connect(csvFileImport, SIGNAL(downloadStarted(int)), this, SLOT(downloadProgressStart(int)));
    connect(csvFileImport, SIGNAL(downloadProgress(int)), this, SLOT(downloadProgress(int)));
    connect(csvFileImport, SIGNAL(downloadEnded(int)), this, SLOT(downloadProgressEnd(int)));
}

MeasuresDownload::~MeasuresDownload() {

    delete withingsDownload;
    delete tredictDownload;
    delete csvFileImport;

}


void
MeasuresDownload::download()
{
    QPointer<MeasuresDownload> guardedDialog(this);
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete =
            guardedContext ? guardedContext->athlete : nullptr;
    const QString groupSymbol = measuresGroupSymbol;
    const QString groupName = measuresGroupName;
    const auto activeGroup =
        [guardedContext, guardedAthlete, groupSymbol]() {
            if (!guardedContext
                || !guardedAthlete
                || guardedContext->athlete
                    != guardedAthlete.data()
                || !guardedAthlete->measures) {
                return static_cast<MeasuresGroup *>(nullptr);
            }
            const int group =
                    guardedAthlete->measures->getGroupSymbols()
                        .indexOf(groupSymbol);
            return guardedAthlete->measures->getGroup(group);
        };

    MeasuresGroup *group = activeGroup();
    if (!group
        || !downloadButton
        || !closeButton
        || !progressBar) {
        reject();
        return;
    }

    downloadButton->setEnabled(false);
    closeButton->setEnabled(false);
    progressBar->setMaximum(1);
    progressBar->setValue(0);

    const QList<Measure> current = group->measures();
    QList<Measure> measures;
    QDateTime fromDate;
    QDateTime toDate;
    QDateTime firstRideDate;
    QString error;
    bool downloadOk = false;

    RideCache *rideCache = guardedAthlete->rideCache;
    if (!rideCache) {
        reject();
        return;
    }
    const QList<QDateTime> rideDates = rideCache->getAllDates();
    if (!guardedDialog || !activeGroup()) return;
    firstRideDate = rideDates.isEmpty()
        ? QDateTime::fromMSecsSinceEpoch(0)
        : rideDates.first();

    if (dateRangeAll->isChecked()) {
        fromDate = firstRideDate;
        toDate = QDateTime::currentDateTimeUtc();
    } else if (dateRangeLastMeasure->isChecked()) {
        fromDate = current.isEmpty()
            ? firstRideDate
            : current.last().when.addSecs(1);
        toDate = QDateTime::currentDateTimeUtc();
    } else if (dateRangeManual->isChecked()) {
        if (manualFromDate->dateTime()
            > manualToDate->dateTime()) {
            QMessageBox::warning(
                this, tr("%1 Measures").arg(groupName),
                tr("Invalid date range - please check your input"));
            if (!guardedDialog) return;
            if (!activeGroup()) {
                reject();
                return;
            }
            downloadButton->setEnabled(true);
            closeButton->setEnabled(true);
            return;
        }
        fromDate.setDate(manualFromDate->date());
        fromDate.setTime(QTime(0, 0));
        toDate.setDate(manualToDate->date());
    } else {
        downloadButton->setEnabled(true);
        closeButton->setEnabled(true);
        return;
    }
    toDate.setTime(QTime(23, 59));

    bool handledByProbe = false;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    if (manualDownloadProbe) {
        handledByProbe = true;
        downloadOk = manualDownloadProbe(
            guardedContext.data(), groupSymbol, error,
            fromDate, toDate, measures);
    }
#endif
    if (!handledByProbe) {
        if (downloadWithings->isChecked()) {
            downloadOk = withingsDownload->getBodyMeasures(
                error, fromDate, toDate, measures);
        } else if (downloadTredict->isChecked()) {
            downloadOk = groupSymbol == QStringLiteral("Hrv")
                ? tredictDownload->getHrvMeasures(
                    error, fromDate, toDate, measures)
                : tredictDownload->getBodyMeasures(
                    error, fromDate, toDate, measures);
        } else if (downloadCSV->isChecked()) {
            downloadOk = csvFileImport->getMeasures(
                group, error, fromDate, toDate, measures);
        } else {
            downloadButton->setEnabled(true);
            closeButton->setEnabled(true);
            return;
        }
    }

    if (!guardedDialog) return;
    group = activeGroup();
    if (!group) {
        reject();
        return;
    }

    if (downloadOk) {
        QMutableListIterator<Measure> iterator(measures);
        while (iterator.hasNext()) {
            const Measure measure = iterator.next();
            if (measure.when <= fromDate
                || measure.when >= toDate) {
                iterator.remove();
            }
        }

        QString updateError;
        downloadOk = updateMeasures(
            guardedContext.data(), group, measures,
            discardExistingMeasures->isChecked(), &updateError);
        if (!downloadOk) error = updateError;

        if (!guardedDialog) return;
        group = activeGroup();
        if (!group) {
            reject();
            return;
        }
    }

    if (downloadOk) {
#ifdef Q_MAC_OS
        QMessageBox messageBox;
        messageBox.setText(tr("Download completed."));
        messageBox.exec();
#endif
    } else {
        if (error.isEmpty())
            error = tr("The operation was cancelled.");
        QMessageBox::warning(
            this, tr("%1 Measures").arg(groupName),
            tr("Downloading or saving measures failed with error: %1")
                .arg(error));
    }

    if (!guardedDialog) return;
    if (!activeGroup()) {
        reject();
        return;
    }
    downloadButton->setEnabled(true);
    closeButton->setEnabled(true);
}

void
MeasuresDownload::close() {

    accept();

}

void
MeasuresDownload::dateRangeAllSettingChanged(bool checked) {

    if (checked && context && context->athlete) {
        manualFromDate->setEnabled(false);
        manualToDate->setEnabled(false);
        appsettings->setCValue(context->athlete->cyclist, GC_BM_LAST_TIMEFRAME, ALL);
    }
}


void
MeasuresDownload::dateRangeLastSettingChanged(bool checked) {

    if (checked && context && context->athlete) {
        manualFromDate->setEnabled(false);
        manualToDate->setEnabled(false);
        appsettings->setCValue(context->athlete->cyclist, GC_BM_LAST_TIMEFRAME, LAST);
    }
}

void
MeasuresDownload::dateRangeManualSettingChanged(bool checked) {

    if (checked && context && context->athlete) {
        manualFromDate->setEnabled(true);
        manualToDate->setEnabled(true);
        appsettings->setCValue(context->athlete->cyclist, GC_BM_LAST_TIMEFRAME, MANUAL);
    }
}

void
MeasuresDownload::downloadWithingsSettingChanged(bool checked) {

    if (checked && context && context->athlete) {
        appsettings->setCValue(context->athlete->cyclist, GC_BM_LAST_TYPE, WITHINGS);
    }
}


void
MeasuresDownload::downloadTredictSettingChanged(bool checked) {

    if (checked && context && context->athlete) {
        appsettings->setCValue(context->athlete->cyclist, GC_BM_LAST_TYPE, TREDICT);
    }
}

void
MeasuresDownload::downloadCSVSettingChanged(bool checked) {

    if (checked && context && context->athlete) {
        appsettings->setCValue(context->athlete->cyclist, GC_BM_LAST_TYPE, CSV);
    }
}


void
MeasuresDownload::downloadProgressStart(int total) {
    if (progressBar) progressBar->setMaximum(total);
}
void
MeasuresDownload::downloadProgress(int current) {
    if (progressBar) progressBar->setValue(current);
}

void
MeasuresDownload::downloadProgressEnd(int final) {
    if (progressBar) progressBar->setValue(final);
}

bool
MeasuresDownload::updateMeasures(Context *context,
                                 MeasuresGroup *measuresGroup,
                                 QList<Measure>&measures,
                                 bool discardExisting,
                                 QString *error) {

   if (error) error->clear();
   const auto fail = [error](const QString &message) {
       if (error) *error = message;
       return false;
   };
   if (!measuresGroup) {
       return fail(tr("The measures group is no longer available."));
   }

   QPointer<Context> guardedContext(context);
   QPointer<Athlete> guardedAthlete =
           guardedContext ? guardedContext->athlete : nullptr;
   if (guardedContext.isNull()
       || guardedAthlete.isNull()
       || guardedContext->athlete != guardedAthlete.data()
       || !guardedAthlete->measures) {
       return fail(tr("The athlete is no longer available."));
   }

   const QString groupSymbol = measuresGroup->getSymbol();
   const QString groupName = measuresGroup->getName();
   const auto activeGroup =
       [guardedContext, guardedAthlete, groupSymbol]() {
           if (!guardedContext
               || !guardedAthlete
               || guardedContext->athlete != guardedAthlete.data()
               || !guardedAthlete->measures) {
               return static_cast<MeasuresGroup *>(nullptr);
           }
           const int group =
                   guardedAthlete->measures->getGroupSymbols()
                       .indexOf(groupSymbol);
           return guardedAthlete->measures->getGroup(group);
       };

   MeasuresGroup *group = activeGroup();
   if (!group || group != measuresGroup) {
       return fail(tr("The measures group is no longer available."));
   }

   const QList<Measure> previous = group->measures();
   QList<Measure> current = previous;

   // we discard only if we have new data loaded - otherwise keep what is there
   if (discardExisting) {
       // now the new measures do not contain any "ts" of the current data any more
       current.clear();
   };

   // if exists, merge current data with new data - new data has preferences
   // no merging of data which has the same time stamp - new wins
   if (current.count() > 0) {
       // remove entry from current list if a new entry with same timestamp exists
       QMutableListIterator<Measure> i(current);
       Measure c;
       while (i.hasNext()) {
           c = i.next();
           foreach(Measure m, measures) {
               if (m.when == c.when) {
                   i.remove();
               }
           }
       }
       // combine the result (without duplicates)
       measures.append(current);
   }

   // update measures in cache and on file store
   RideCache *rideCache = guardedAthlete->rideCache;
   if (!rideCache) {
       return fail(tr("The activity cache is no longer available."));
   }
   rideCache->cancel();
   group = activeGroup();
   if (!group) {
       return fail(tr("The operation was cancelled."));
   }

   // store in athlete
   group->setMeasures(measures);

   QString writeError;
   if (!group->write(&writeError)) {
       if (MeasuresGroup *rollbackGroup = activeGroup()) {
           QList<Measure> rollback = previous;
           rollbackGroup->setMeasures(rollback);
       }
       if (guardedAthlete && guardedAthlete->rideCache) {
           guardedAthlete->rideCache->refresh();
       }
       const QString message =
           tr("Saving downloaded measures failed: %1")
               .arg(writeError.isEmpty()
                        ? tr("Unknown error")
                        : writeError);
       qWarning() << tr("%1 Measures")
           .arg(groupName) << message;
       return fail(message);
   }
   if (!activeGroup()) {
       return fail(tr("The operation was cancelled."));
   }

   // do a refresh, it will check if needed
   rideCache = guardedAthlete->rideCache;
   if (!rideCache) {
       return fail(tr("The activity cache is no longer available."));
   }
   rideCache->refresh();
   if (!activeGroup()) {
       return fail(tr("The operation was cancelled."));
   }
   return true;
}

void MeasuresDownload::autoDownload(Context *context) {
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete =
            guardedContext ? guardedContext->athlete : nullptr;
    if (guardedContext.isNull()
        || guardedAthlete.isNull()
        || guardedContext->athlete != guardedAthlete.data()) {
        return;
    }

    CloudServiceFactory &factory = CloudServiceFactory::instance();

    // iterate over names, as they are sorted alphabetically
    foreach(QString name, factory.serviceNames()) {

        // get the service
        const CloudService *s = factory.service(name);
        if (!s) continue;
        const auto execution = s->startupMeasuresExecution();

        // only ones with the capability we need.
        if (!(s->type() & CloudService::Measures)
            || !(s->capabilities() & CloudService::Download)
            || !s->supportsStartupMeasuresDownload()) {
            continue;
        }
        // only ones with Sync on Startup enabled
        if (appsettings->cvalue(context->athlete->cyclist, s->syncOnStartupSettingName(), "false").toString() != "true") continue;

        bool authorized = false;
        switch (execution) {
        case CloudService::StartupMeasuresExecution::Withings:
            authorized = !appsettings->cvalue(
                context->athlete->cyclist,
                GC_NOKIA_REFRESH_TOKEN, "").toString().isEmpty();
            break;
        case CloudService::StartupMeasuresExecution::Tredict:
            authorized = !appsettings->cvalue(
                context->athlete->cyclist,
                GC_TREDICT_REFRESH_TOKEN, "").toString().isEmpty();
            break;
        case CloudService::StartupMeasuresExecution::Unsupported:
            break;
        }
        if (!authorized) continue;

        // iterate over measures groups
        foreach(MeasuresGroup* measuresGroup, context->athlete->measures->getGroups()) {

            QString group = measuresGroup->getSymbol();
            // Only supported groups
            if (group != "Body" && group != "Hrv") continue;

            const bool groupSupported =
                (execution
                    == CloudService::StartupMeasuresExecution::Withings
                 && group == "Body")
                || (execution
                    == CloudService::StartupMeasuresExecution::Tredict
                    && (group == "Body" || group == "Hrv"));
            if (!groupSupported) continue;

            QList<Measure> current = measuresGroup->measures();
            QList<Measure> measures;
            QDateTime fromDate;
            QDateTime toDate;
            QDateTime firstRideDate;
            QString err = "";
            bool downloadOk = false;

            // get the date of first ride as potential "from" value
            QList<QDateTime> rideDates = context->athlete->rideCache->getAllDates();
            if (rideDates.count() > 0) {
                firstRideDate = rideDates.at(0);
            }  else {
                firstRideDate = QDateTime::fromMSecsSinceEpoch(0);
            }

            // determine the date range
            if (current.count() > 0) {
                fromDate = current.last().when.addSecs(1);
            } else {
                // use a reasonable default
                fromDate = firstRideDate;
            }
            toDate = QDateTime::currentDateTimeUtc();
            // to Time is always "end of the day"
            toDate.setTime(QTime(23,59));

            // do the download
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            if (autoDownloadProbe) {
                downloadOk = autoDownloadProbe(
                    context, group, err, fromDate, toDate, measures);
            } else
#endif
            switch (execution) {
            case CloudService::StartupMeasuresExecution::Withings:
                if (group == "Body") {
                    downloadOk = WithingsDownload(context).getBodyMeasures(
                        err, fromDate, toDate, measures);
                }
                break;
            case CloudService::StartupMeasuresExecution::Tredict:
                if (group == "Body") {
                    downloadOk = TredictMeasuresDownload(context).getBodyMeasures(
                        err, fromDate, toDate, measures);
                } else if (group == "Hrv") {
                    downloadOk = TredictMeasuresDownload(context).getHrvMeasures(
                        err, fromDate, toDate, measures);
                }
                break;
            case CloudService::StartupMeasuresExecution::Unsupported:
                break;
            }

            if (guardedContext.isNull()
                || guardedAthlete.isNull()
                || guardedContext->athlete
                    != guardedAthlete.data()) {
                return;
            }

            if (downloadOk) {
                // selection from various source may not be 100% accurate w.r.t. the from/to date filtering
                // so remove all measures which do not fit the selection from/to interval
                QMutableListIterator<Measure> i(measures);
                Measure c;
                while (i.hasNext()) {
                    c = i.next();
                    if (c.when <= fromDate || c.when >= toDate) {
                        i.remove();
                    }
                }

                if (!updateMeasures(
                        guardedContext.data(), measuresGroup,
                        measures, false)
                    && (guardedContext.isNull()
                        || guardedAthlete.isNull()
                        || guardedContext->athlete
                            != guardedAthlete.data())) {
                    return;
                }

            } else if (!err.isEmpty()){
                // handle error document in err String
                qDebug() << tr("%1 Measures").arg(measuresGroup->getName()) << tr("Downloading of measures failed with error: %1").arg(err);
            }
        }
    }
}
