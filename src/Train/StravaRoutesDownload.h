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
#include "StravaRoutesDownloadPipeline.h"

#include <QtGui>
#include <QTableWidget>
#include <QProgressBar>
#include <QList>
#include <QLabel>
#include <QListIterator>
#include <QPointer>
#include <QDebug>

#include <functional>
#include <memory>

// Dialog class to show filenames, import progress and to capture user input
// of ride date and time

class Strava;
class TrainDB;
struct GpxParserOptions;

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
namespace StravaRoutesDownloadProductionTest {
class Access;
}
#endif

class StravaRoutesDownload : public QDialog
{
#ifndef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
    Q_OBJECT
    G_OBJECT
#endif


public:
    StravaRoutesDownload(Context *context);
    ~StravaRoutesDownload() override;

    QTreeWidget *files; // choose files to export

#ifndef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
private slots:
#else
private:
#endif
    void cancelClicked();
    void downloadClicked();
    void allClicked();
    void refreshClicked();

private:
    struct ImportOperation;
#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
    struct TestConstruction {};
    explicit StravaRoutesDownload(TestConstruction);
    friend class StravaRoutesDownloadProductionTest::Access;
#endif

    void connectAbortAndCloseActions();
    void beginSelectedDownloads();
    void startDownloadBatch();
    void startImportBatch(
        const StravaRoutesDownloadPipeline::DownloadBatchResult &result);
    void finishPreparationBatch(
        const QList<StravaRoutesDownloadPipeline::RouteFailure> &failures,
        const QStringList &cancelledRouteIds,
        const QString &error,
        bool operationFailed);
    void startDecisionCommit();
    void finishDecisionCommit(
        bool committed,
        bool recoveryRequired,
        const QString &error,
        bool operationFailed);
    void startPublication();
    void finishPublication(
        bool published,
        const QString &error,
        bool operationFailed);
    void startFinalization();
    void finishFinalization(
        std::shared_ptr<ImportOperation> operation,
        bool committed,
        bool stagingCleaned,
        const QString &error,
        bool operationFailed);
    void continueAfterImport(
        const StravaRoutesDownloadPipeline::DownloadBatchResult &result);
    void finishStagingCleanup(
        const StravaRoutesDownloadPipeline::DownloadBatchResult &result,
        bool cleaned,
        bool operationFailed);
    void finishDownload();
    void finishRefresh(
        const StravaRoutesClient::RoutesResult &result,
        bool operationFailed);
    void finishDownloadBatch(
        const StravaRoutesDownloadPipeline::DownloadBatchResult &result,
        bool operationFailed);
    void contextClosing();
    Strava *createStravaService() const;
    QTreeWidgetItem *itemForRoute(const QString &routeId) const;
    void setRouteStatus(const QString &routeId,
                        const QString &text,
                        const QString &toolTip = QString());
    void closeEvent(QCloseEvent *event) override;

    QPointer<Context> context;
    bool busy;
    bool overwriteExisting;

    QCheckBox *all;
    QCheckBox *overwrite;

    QPushButton *close, *download, *refreshButton;

    int downloads, fails;
    QLabel *status;

    StravaRoutesDownloadPipeline::ImportLifecycle lifecycle;
    StravaRoutesDownloadPipeline::Runner *activeRunner;
    std::shared_ptr<ImportOperation> importOperation;
    QStringList pendingRouteIds;
    std::function<void()> beginDownloadAction;
    std::function<void(
        const StravaRoutesDownloadPipeline::DownloadBatchResult &,
        bool,
        bool)> finishStagingAction;
};

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
namespace StravaRoutesDownloadProductionTest {

class ImportOperation;
using ImportOperationPtr = std::shared_ptr<ImportOperation>;

ImportOperationPtr prepareImport(
    const StravaRoutesDownloadPipeline::DownloadBatchResult &download,
    const QString &athleteRoot,
    const QString &workoutRoot,
    bool overwriteExisting,
    const GpxParserOptions &parserOptions,
    const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
    QString &error);
bool commitDecision(
    const ImportOperationPtr &operation,
    const QString &databasePath,
    const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
    bool &committed,
    QString &error);
bool publish(
    const ImportOperationPtr &operation,
    const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
    QString &error);
bool finalize(
    const ImportOperationPtr &operation,
    bool &stagingCleaned,
    QString &error);
bool finalizeWithDatabase(
    const ImportOperationPtr &operation,
    TrainDB *database,
    bool &stagingCleaned,
    QString &error);
bool notifyFinalization(
    const ImportOperationPtr &operation,
    const std::function<void()> &notification);
StravaRoutesDownload *createDialog();
void configureBusy(StravaRoutesDownload *dialog, bool busy);
void clickAbort(StravaRoutesDownload *dialog);
void clickClose(StravaRoutesDownload *dialog);
bool aborted(const StravaRoutesDownload *dialog);
bool closeWhenIdle(const StravaRoutesDownload *dialog);
void attachOperation(
    StravaRoutesDownload *dialog,
    const ImportOperationPtr &operation);
void finishDialogFinalization(
    StravaRoutesDownload *dialog,
    bool committed,
    bool stagingCleaned,
    const QString &error,
    bool operationFailed);

} // namespace StravaRoutesDownloadProductionTest
#endif

#endif // _StravaRoutesDownload_h
