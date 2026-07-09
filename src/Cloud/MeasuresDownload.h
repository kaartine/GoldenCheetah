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

#ifndef _Gc_MeasuresDownload_h
#define _Gc_MeasuresDownload_h

#include "Context.h"

#include "WithingsDownload.h"
#include "TredictMeasuresDownload.h"
#include "MeasuresCsvImport.h"

#include <QDialog>
#include <QCheckBox>
#include <QRadioButton>
#include <QDateEdit>
#include <QLabel>
#include <QProgressBar>
#include <QPointer>

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
#include <functional>
#endif


class MeasuresDownload : public QDialog
{
    Q_OBJECT

public:
    MeasuresDownload(Context *context, MeasuresGroup *measuresGroup);
    ~MeasuresDownload();
    static bool updateMeasures(Context *context,
                               MeasuresGroup *measuresGroup,
                               QList<Measure>&measures,
                               bool discardExisting=false,
                               QString *error=nullptr);
    static void autoDownload(Context *context);

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    using AutoDownloadProbe = std::function<bool(
        Context *, const QString &, QString &,
        const QDateTime &, const QDateTime &,
        QList<Measure> &)>;
    using ManualDownloadProbe = AutoDownloadProbe;
    static void setAutoDownloadProbeForTest(AutoDownloadProbe probe);
    static void setManualDownloadProbeForTest(ManualDownloadProbe probe);
#endif

private:

     QPointer<Context> context;

     MeasuresGroup *measuresGroup = nullptr;
     QString measuresGroupSymbol;
     QString measuresGroupName;

     WithingsDownload *withingsDownload = nullptr;
     TredictMeasuresDownload *tredictDownload = nullptr;
     MeasuresCsvImport *csvFileImport = nullptr;

     QPushButton *downloadButton = nullptr;
     QPushButton *closeButton = nullptr;

     QCheckBox *discardExistingMeasures = nullptr;

     // withings, tredict, csv file
     QRadioButton *downloadWithings = nullptr;
     QRadioButton *downloadTredict = nullptr;
     QRadioButton *downloadCSV = nullptr;

     //  all, from last measure, manual date interval
     QRadioButton *dateRangeAll = nullptr;
     QRadioButton *dateRangeLastMeasure = nullptr;
     QRadioButton *dateRangeManual = nullptr;

     QDateEdit *manualFromDate = nullptr;
     QDateEdit *manualToDate = nullptr;

     // Progress Bar
     QProgressBar *progressBar = nullptr;

     enum source { WITHINGS = 1,
                   TP = 2,
                   CSV = 3,
                   TREDICT = 4
                 } ;

     enum timeframe { ALL = 1,
                      LAST = 2,
                      MANUAL = 3
                    } ;

private slots:
     void download();
     void close();
     void dateRangeAllSettingChanged(bool);
     void dateRangeLastSettingChanged(bool);
     void dateRangeManualSettingChanged(bool);

     void downloadWithingsSettingChanged(bool);
     void downloadTredictSettingChanged(bool);
     void downloadCSVSettingChanged(bool);

     void downloadProgressStart(int);
     void downloadProgress(int);
     void downloadProgressEnd(int);

};


#endif
