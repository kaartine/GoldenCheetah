/*
 * Copyright (c) 2017 Joern Rischmueller (joern.rm@gmail.com)
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

#include "StravaRoutesDownload.h"
#include "MainWindow.h"
#include "TrainDB.h"
#include "HelpWhatsThis.h"
#include "TimeUtils.h"
#include "CloudService.h"
#include "Strava.h"

#include <QCloseEvent>
#include <QDir>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTimer>

StravaRoutesDownload::StravaRoutesDownload(Context *context)
    : QDialog(context->mainWindow),
      context(context),
      aborted(false),
      busy(false),
      closeWhenIdle(false)
{
    CloudService *configured =
        CloudServiceFactory::instance().newService(
            QStringLiteral("Strava"), context);
    Strava *service = dynamic_cast<Strava *>(configured);
    if (service) {
        stravaService.reset(service);
    } else {
        delete configured;
    }
    routesClient = std::make_unique<StravaRoutesClient>(
        [this](
            const QUrl &url,
            qsizetype maximumBytes,
            const StravaRoutesClient::CancellationCheck &cancelled) {
            if (!stravaService) {
                StravaAuthenticatedSession::Result result;
                result.error = tr(
                    "The Strava service is unavailable.");
                return result;
            }
            return stravaService->authenticatedGet(
                url, maximumBytes, cancelled);
        });

    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setWindowTitle(tr("Download Routes as workouts from Strava"));

    // help
    HelpWhatsThis *help = new HelpWhatsThis(this);
    this->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::MenuBar_Tools_Download_StravaRoutes));

    // make the dialog a resonable size
    setMinimumWidth(650);
    setMinimumHeight(400);

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    // TopLine Fields and Layout

    all = new QCheckBox(tr("check/uncheck all"), this);
    all->setChecked(true);

    refreshButton = new QPushButton(tr("Refresh List"), this);

    QHBoxLayout *topline = new QHBoxLayout;
    topline->addWidget(all);
    topline->addStretch();
    topline->addStretch();
    topline->addWidget(refreshButton);

    // Filelist Widget

    files = new QTreeWidget;
#ifdef Q_OS_MAC
    files->setAttribute(Qt::WA_MacShowFocusRect, 0);
#endif
    files->headerItem()->setText(0, tr(""));
    files->headerItem()->setText(1, tr("Name"));
    files->headerItem()->setText(2, tr("Description"));
    files->headerItem()->setText(3, tr("Action"));

    files->setColumnCount(4);
    files->setColumnWidth(0, 30*dpiXFactor); // selector
    files->setColumnWidth(1, 220*dpiXFactor); // name
    files->setColumnWidth(2, 240*dpiXFactor); // description
    files->setSelectionMode(QAbstractItemView::SingleSelection);
    files->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    files->setUniformRowHeights(true);
    files->setIndentation(0);

    // BottomLine Fields and Layout
    QHBoxLayout *bottomLine = new QHBoxLayout;
    status = new QLabel("", this);
    status->hide();
    overwrite = new QCheckBox(tr("Overwrite existing workouts"), this);
    close = new QPushButton(tr("Close"), this);
    download = new QPushButton(tr("Download"), this);
    bottomLine->addWidget(overwrite);
    bottomLine->addWidget(status);
    bottomLine->addStretch();
    bottomLine->addWidget(close);
    bottomLine->addWidget(download);

    layout->addLayout(topline);
    layout->addWidget(files);
    layout->addLayout(bottomLine);

    // connect signals and slots up..
    connect(download, SIGNAL(clicked()), this, SLOT(downloadClicked()));
    connect(all, SIGNAL(stateChanged(int)), this, SLOT(allClicked()));
    connect(close, SIGNAL(clicked()), this, SLOT(cancelClicked()));
    connect(refreshButton, SIGNAL(clicked()), this, SLOT(refreshClicked()));

    QTimer::singleShot(
        0, this, &StravaRoutesDownload::refreshClicked);

}

StravaRoutesDownload::~StravaRoutesDownload() = default;

void
StravaRoutesDownload::allClicked()
{
    // set/uncheck all rides according to the "all"
    bool checked = all->isChecked();

    for(int i=0; i<files->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *current = files->invisibleRootItem()->child(i);
        static_cast<QCheckBox*>(files->itemWidget(current,0))->setChecked(checked);
    }
}

void
StravaRoutesDownload::downloadClicked()
{
    if (download->text() == tr("Download")) {
        downloads = fails = 0;
        aborted = false;
        busy = true;
        refreshButton->setEnabled(false);
        overwrite->hide();
        status->setText(tr("Download..."));
        status->show();
        close->hide();
        download->setText(tr("Abort"));
        downloadFiles();
        status->setText(QString(tr("%1 workouts downloaded, %2 failed or skipped.")).arg(downloads).arg(fails));
        download->setText(tr("Download"));
        download->setEnabled(true);
        close->show();
        close->setEnabled(true);
        overwrite->show();
        refreshButton->setEnabled(true);
        busy = false;

        context->notifyWorkoutsChanged();
        if (closeWhenIdle) {
            reject();
        }

    } else if (download->text() == tr("Abort")) {
        aborted = true;
        download->setEnabled(false);
        status->setText(tr("Cancelling..."));
    }
}

void
StravaRoutesDownload::cancelClicked()
{
    if (busy) {
        aborted = true;
        closeWhenIdle = true;
        close->setEnabled(false);
        status->setText(tr("Cancelling..."));
        status->show();
        return;
    }
    reject();
}



void
StravaRoutesDownload::refreshClicked()
{
    if (busy) return;

    // reset download information
    aborted = false;
    busy = true;
    closeWhenIdle = false;
    status->setText("");
    downloads = fails = 0;
    download->setEnabled(false);
    download->show();
    close->show();
    close->setEnabled(true);
    refreshButton->setEnabled(false);
    files->clear(); // delete existing entries
    QString error;

    const QList<StravaRouteSummary> routes =
        getFileList(error);
    busy = false;
    download->setEnabled(true);
    refreshButton->setEnabled(true);
    if (closeWhenIdle) {
        reject();
        return;
    }
    if (error != "") {
        QMessageBox::warning(this, tr("Today's Plan Workout Download"), QString(tr("The following error occured: %1").arg(error)));
        return;
    }


    for (const StravaRouteSummary &item : routes) {

       QTreeWidgetItem *add = new QTreeWidgetItem(files->invisibleRootItem());
       add->setFlags(add->flags() | Qt::ItemIsEditable);

       // selector
       QCheckBox *checkBox = new QCheckBox("", this);
       checkBox->setChecked(true);
       files->setItemWidget(add, 0, checkBox);

       add->setText(1, item.name);
       add->setText(2, item.description);

       // interval action
       add->setText(3, tr("Download"));

       add->setData(0, Qt::UserRole, item.routeId);
    }
}

void
StravaRoutesDownload::downloadFiles()
{
    const QString workoutDir =
        appsettings->value(this, GC_WORKOUTDIR).toString();
    const QDir temporaryDirectory =
        context->athlete->home->temp();

    // loop through the table and export all selected
    for(int i=0; i<files->invisibleRootItem()->childCount(); i++) {

        // give user a chance to abort..
        QApplication::processEvents();

        // did they?
        if (aborted == true) {
            return; // user aborted!
        }

        QTreeWidgetItem *current = files->invisibleRootItem()->child(i);

        // is it selected
        if (static_cast<QCheckBox*>(files->itemWidget(current,0))->isChecked()) {

            files->setCurrentItem(current); QApplication::processEvents();

            // this one then
            current->setText(3, tr("Downloading...")); QApplication::processEvents();

            const QString routeId =
                current->data(0, Qt::UserRole).toString();
            QByteArray content;
            QString error;
            if (!readFile(
                    &content, routeId, &error)) {
                current->setText(3,tr("Error downloading")); QApplication::processEvents();
                current->setToolTip(3, error);
                fails++;
                continue;

            }

            const QString fileBasename =
                StravaRoutesClient::workoutFileName(routeId);
            if (fileBasename.isEmpty()) {
                current->setText(3, tr("Invalid File"));
                fails++;
                continue;
            }
            const QString filename =
                QDir(workoutDir).filePath(fileBasename);

            QTemporaryFile temporaryFile(
                temporaryDirectory.filePath(
                    QStringLiteral(
                        "Strava-Route-XXXXXX.gpx")));
            if (!temporaryFile.open()
                || temporaryFile.write(content)
                    != content.size()) {
                temporaryFile.remove();
                current->setText(3, tr("Write failed"));
                fails++;
                continue;
            }
            const QString temporaryPath =
                temporaryFile.fileName();
            temporaryFile.close();

            std::unique_ptr<ErgFile> workout =
                std::make_unique<ErgFile>(
                    temporaryPath,
                    ErgFileFormat::crs,
                    context);

            // now zap the temporary file
            temporaryFile.remove();

            // open success?
            if (workout->isValid()) {


                if (QFile(filename).exists()) {

                   if (overwrite->isChecked() == false) {
                        // skip existing files
                        current->setText(3,tr("Exists already")); QApplication::processEvents();
                        fails++;
                        continue;

                    } else {

                        current->setText(3, tr("Removing...")); QApplication::processEvents();
                    }

                }

                QSaveFile out(filename);
                if (out.open(QIODevice::WriteOnly) == true) {
                    const bool saved =
                        out.write(content) == content.size()
                        && out.commit();
                    if (!saved) {
                        fails++;
                        current->setText(
                            3, tr("Write failed"));
                        continue;
                    }

                    downloads++;
                    current->setText(3, tr("Saved")); QApplication::processEvents();
                    trainDB->startLUW();
                    trainDB->importWorkout(
                        filename, *workout);
                    trainDB->endLUW();

                } else {

                    fails++;
                    current->setText(3, tr("Write failed")); QApplication::processEvents();
                }

            // couldn't parse
            } else {

                fails++;
                current->setText(3, tr("Invalid File")); QApplication::processEvents();

            }

        }
    }
}

QList<StravaRouteSummary>
StravaRoutesDownload::getFileList(QString &error)
{
    const StravaRoutesClient::RoutesResult result =
        routesClient->listRoutes(
            [this] { return aborted; });
    if (!result.isValid())
        error = result.error;
    return result.routes;
}

bool
StravaRoutesDownload::readFile(
    QByteArray *data,
    const QString &routeId,
    QString *error)
{
    if (!data) return false;
    const StravaRoutesClient::PayloadResult result =
        routesClient->downloadGpx(
            routeId, [this] { return aborted; });
    if (!result.isValid()) {
        if (error) *error = result.error;
        return false;
    }
    *data = result.payload;
    return true;
}

void
StravaRoutesDownload::closeEvent(QCloseEvent *event)
{
    if (!busy) {
        QDialog::closeEvent(event);
        return;
    }

    aborted = true;
    closeWhenIdle = true;
    close->setEnabled(false);
    download->setEnabled(false);
    status->setText(tr("Cancelling..."));
    status->show();
    event->ignore();
}
