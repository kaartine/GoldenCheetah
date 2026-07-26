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

#ifndef _StravaRoutesDownload_h
#define _StravaRoutesDownload_h
#include "GoldenCheetah.h"
#include "Context.h"
#include "Settings.h"
#include "Units.h"

#include "TrainSidebar.h"
#include "StravaRoutesClient.h"

#include <QtGui>
#include <QTableWidget>
#include <QProgressBar>
#include <QList>
#include <QLabel>
#include <QListIterator>
#include <QDebug>

#include <memory>

// Dialog class to show filenames, import progress and to capture user input
// of ride date and time

class Strava;

class StravaRoutesDownload : public QDialog
{
    Q_OBJECT
    G_OBJECT


public:
    StravaRoutesDownload(Context *context);
    ~StravaRoutesDownload() override;

    QTreeWidget *files; // choose files to export

signals:

private slots:
    void cancelClicked();
    void downloadClicked();
    void allClicked();
    void refreshClicked();

private:

    void downloadFiles();
    QList<StravaRouteSummary> getFileList(QString &error);
    bool readFile(
        QByteArray *data,
        const QString &routeId,
        QString *error);
    void closeEvent(QCloseEvent *event) override;

    Context *context;
    bool aborted;
    bool busy;
    bool closeWhenIdle;

    QCheckBox *all;

    QCheckBox *overwrite;
    QPushButton *close, *download, *refreshButton;

    int downloads, fails;
    QLabel *status;

    std::unique_ptr<Strava> stravaService;
    std::unique_ptr<StravaRoutesClient> routesClient;
};
#endif // _StravaRoutesDownload_h
