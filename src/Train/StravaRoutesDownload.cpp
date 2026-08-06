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
#include "CloudService.h"
#include "Strava.h"
#include "FileIO/AnchoredFileSystem.h"
#include "FileIO/GpxParser.h"
#include "Planning/PlanBundleImportJournal.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

using StravaRoutesDownloadPipeline::DownloadBatchResult;
using StravaRoutesDownloadPipeline::RouteFailure;
using StravaRoutesDownloadPipeline::Runner;
using StravaRoutesDownloadPipeline::StagedRoute;

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
extern void stravaRoutesDownloadFinishStagingCleanupTestHook();
#endif

namespace {

template<typename Result>
class SharedResult
{
public:
    void store(Result result)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        value = std::move(result);
    }

    Result load() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return value;
    }

private:
    mutable std::mutex mutex;
    Result value{};
};

class RouteImportState
{
public:
    RouteImportState(QString athleteRoot,
                     QString workoutDirectory,
                     bool overwriteExisting,
                     GpxParserOptions parserOptions)
        : athleteRoot(std::move(athleteRoot))
        , workoutDirectory(std::move(workoutDirectory))
        , overwriteExisting(overwriteExisting)
        , parserOptions(std::move(parserOptions))
    {
    }

    struct PreparedRoute
    {
        StagedRoute staged;
        QString targetFileName;
        QString targetPath;
        QByteArray contents;
        std::unique_ptr<ErgFile> workout;
        bool replaceExisting = false;
        qint64 previousSize = -1;
        QByteArray previousDigest;
    };

    bool prepare(
        const StagedRoute &route,
        const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
        QString &error)
    {
        const QString fileName =
            StravaRoutesClient::workoutFileName(route.routeId);
        if (fileName.isEmpty()) {
            error = QStringLiteral("The Strava route target is invalid.");
            return false;
        }

        if ((!workoutRootAnchor.isValid()
             && !AnchoredFileSystem::DirectoryAnchor::open(
                workoutDirectory, workoutRootAnchor, error))
            || !workoutRootAnchor.pathMatches(error)
            || !AnchoredFileSystem::validateCurrentUserControlledDirectory(
                workoutRootAnchor, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The workout directory is unavailable.");
            }
            return false;
        }
        const AnchoredFileSystem::EntryRef targetEntry =
            workoutRootAnchor.entry(fileName, error);
        bool targetExists = false;
        if (!targetEntry.isValid()
            || !AnchoredFileSystem::entryExists(
                targetEntry, targetExists, error)) {
            return false;
        }
        AnchoredFileSystem::PinnedFile previousTarget;
        if (targetExists && !overwriteExisting) {
            error = QStringLiteral("The workout already exists.");
            return false;
        }
        if (targetExists
            && !StravaRoutesDownloadPipeline::pinRoutePredecessor(
                targetEntry, previousTarget, error, cancelled)) {
            return false;
        }

        const QString targetPath =
            QDir(workoutDirectory).absoluteFilePath(fileName);
        if (find(route.routeId) || containsTarget(targetPath)) {
            error = QStringLiteral(
                "The Strava route target is duplicated.");
            return false;
        }

        QByteArray contents;
        std::unique_ptr<ErgFile> workout;
        if (!StravaRoutesDownloadPipeline::withPinnedRouteBytes(
                route,
                [this, &contents, &workout, &targetPath, &cancelled](
                    const QByteArray &source, QString &parseError) {
                    contents = source;
                    workout.reset(ErgFile::fromGpxContentBytes(
                        source, targetPath, ErgFileFormat::crs,
                        nullptr, parserOptions, parseError, cancelled));
                    return bool(workout);
                },
                error, cancelled)) {
            return false;
        }
        if (targetExists) {
            bool targetMatches = false;
            if (!AnchoredFileSystem::entryMatches(
                    targetEntry, previousTarget, targetMatches, error)
                || !targetMatches) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "The workout target changed during validation.");
                }
                return false;
            }
        } else {
            bool targetStillExists = false;
            if (!AnchoredFileSystem::entryExists(
                    targetEntry, targetStillExists, error)
                || targetStillExists) {
                error = QStringLiteral(
                    "The workout target changed during validation.");
                return false;
            }
        }

        auto entry = std::make_unique<PreparedRoute>();
        entry->staged = route;
        entry->targetFileName = fileName;
        entry->targetPath = targetPath;
        entry->contents = std::move(contents);
        entry->workout = std::move(workout);
        entry->replaceExisting = targetExists;
        if (targetExists) {
            entry->previousSize = previousTarget.size();
            entry->previousDigest = previousTarget.sha256();
        }
        entries.push_back(std::move(entry));
        return true;
    }

    bool prepareJournal(QString &error)
    {
        if (entries.empty()) return true;
        QList<TrainDB::PlanImportWorkout> records;
        for (const auto &entry : entries) {
            TrainDB::PlanImportWorkout record;
            record.targetFileName = entry->targetFileName;
            record.contents = entry->contents;
            record.digest = entry->staged.digest;
            record.replaceExisting = entry->replaceExisting;
            record.previousSize = entry->previousSize;
            record.previousDigest = entry->previousDigest;
            records.append(std::move(record));
        }
        journal = PlanBundleImport::Journal::createStandalonePrepared(
            athleteRoot, workoutDirectory, workoutRootAnchor,
            records, error);
        return bool(journal);
    }

    bool commitDecision(
        const QString &databasePath,
        const std::shared_ptr<TrainDatabaseFileGeneration>
            &databaseGeneration,
        const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
        QString &error,
        bool &recoveryRequired)
    {
        recoveryRequired = false;
        if (entries.empty()) return true;
        if (!journal || databasePath.isEmpty() || !databaseGeneration) {
            error = QStringLiteral(
                "The workout database is unavailable.");
            return false;
        }

        bool decisionCommitted = false;
        const bool decisionSucceeded =
            journal->commitStandaloneDecision(
                databasePath, cancelled, databaseGeneration,
                decisionCommitted, error);
        if (!decisionSucceeded) {
            recoveryRequired = decisionCommitted;
            return false;
        }
        recoveryRequired = true;
        databasePath_ = databasePath;
        databaseGeneration_ = databaseGeneration;
        return true;
    }

    bool publish(
        const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
        QString &error)
    {
        if (!journal) {
            error = QStringLiteral(
                "The Strava import journal is unavailable.");
            return false;
        }
        return journal->publishPreparedFiles(cancelled, error);
    }

    bool commitDatabase(TrainDB *database, QString &error)
    {
        if (!journal || !database
            || QThread::currentThread() != database->thread()) {
            error = QStringLiteral(
                "The workout database is unavailable.");
            return false;
        }

        if (!journal->bindDatabase(database, error)) return false;
        const PlanBundleImport::BoundDatabaseCompletion completion =
            [this, database](
                const TrainDB::PlanImportJournal &record,
                const PlanBundleImport::PublishedValidation
                    &validatePublished,
                QString &completionError) {
                if (!database
                    || QThread::currentThread() != database->thread()
                    || record.workouts.size()
                        != qsizetype(entries.size())) {
                    completionError = QStringLiteral(
                        "The Strava import owners changed before commit.");
                    return false;
                }

                TrainDB::ScopedLUW transaction(*database);
                if (!transaction.isActive()) {
                    completionError = QStringLiteral(
                        "The Strava route import transaction could not be started.");
                    return false;
                }
                for (const auto &entry : entries) {
                    if (!database->importWorkout(
                            entry->targetPath,
                            *entry->workout,
                            ImportMode::insertOrUpdate)) {
                        completionError = QStringLiteral(
                            "The Strava route could not be imported.");
                        return false;
                    }
                }
                if (!validatePublished(completionError)
                    || !database->removePlanImportJournal(
                        record.id, completionError)
                    || !transaction.commit()) {
                    return false;
                }
                return true;
            };
        return journal->completePublishedDatabaseBound(completion, error);
    }

    bool commitDatabaseAtPath(QString &error)
    {
        if (databasePath_.isEmpty() || !databaseGeneration_
            || !TrainDB::databaseFileGenerationMatches(
                databasePath_, databaseGeneration_, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The workout database generation is unavailable.");
            }
            return false;
        }
        TrainDB worker(
            QDir(QFileInfo(databasePath_).absolutePath()),
            QStringLiteral("strava-finalize-%1").arg(
                QUuid::createUuid().toString(QUuid::WithoutBraces)),
            false, databaseGeneration_);
        const TrainDB::SchemaStatus status = worker.schemaStatus();
        if ((status != TrainDB::SchemaStatus::current
             && status != TrainDB::SchemaStatus::migrationReady)
            || worker.databaseFilePath()
                != QFileInfo(databasePath_).absoluteFilePath()
            || !TrainDB::databaseFileGenerationMatches(
                databasePath_, databaseGeneration_, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The workout database changed before finalization.");
            }
            return false;
        }
        if (!commitDatabase(&worker, error)) return false;
        return TrainDB::databaseFileGenerationMatches(
            databasePath_, databaseGeneration_, error);
    }

    QStringList routeIds() const
    {
        QStringList result;
        for (const auto &entry : entries)
            result.append(entry->staged.routeId);
        return result;
    }

private:
    PreparedRoute *find(const QString &routeId) const
    {
        for (const auto &entry : entries) {
            if (entry->staged.routeId == routeId) return entry.get();
        }
        return nullptr;
    }

    bool containsTarget(const QString &targetPath) const
    {
        for (const auto &entry : entries) {
            if (entry->targetPath == targetPath) return true;
        }
        return false;
    }

    QString athleteRoot;
    QString workoutDirectory;
    bool overwriteExisting = false;
    GpxParserOptions parserOptions;
    AnchoredFileSystem::DirectoryAnchor workoutRootAnchor;
    std::vector<std::unique_ptr<PreparedRoute>> entries;
    std::shared_ptr<PlanBundleImport::Journal> journal;
    QString databasePath_;
    std::shared_ptr<TrainDatabaseFileGeneration> databaseGeneration_;
};

struct PreparationResult
{
    QList<RouteFailure> failures;
    QStringList cancelledRouteIds;
    QString error;
};

struct PublicationResult
{
    bool published = false;
    QString error;
};

struct DecisionResult
{
    bool committed = false;
    bool recoveryRequired = false;
    QString error;
};

struct FinalizationResult
{
    bool committed = false;
    bool stagingCleaned = false;
    QString error;
};

} // namespace

struct StravaRoutesDownload::ImportOperation
{
    explicit ImportOperation(
        const DownloadBatchResult &download,
        const QString &athleteRoot,
        const QString &workoutDirectory,
        bool overwriteExisting,
        const GpxParserOptions &parserOptions)
        : download(download)
        , state(std::make_shared<RouteImportState>(
              athleteRoot, workoutDirectory, overwriteExisting,
              parserOptions))
    {
    }

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
    ImportOperation(
        DownloadBatchResult download,
        std::shared_ptr<RouteImportState> state)
        : download(std::move(download)), state(std::move(state))
    {
    }
#endif

    DownloadBatchResult download;
    std::shared_ptr<RouteImportState> state;
};

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
namespace StravaRoutesDownloadProductionTest {

class ImportOperation
{
public:
    ImportOperation(
        DownloadBatchResult download,
        std::shared_ptr<RouteImportState> state)
        : download(std::move(download)), state(std::move(state))
    {
    }

    DownloadBatchResult download;
    std::shared_ptr<RouteImportState> state;
};

class Access
{
public:
    static StravaRoutesDownload *createDialog()
    {
        return new StravaRoutesDownload(
            StravaRoutesDownload::TestConstruction{});
    }

    static void configureBusy(StravaRoutesDownload *dialog, bool value)
    {
        if (!dialog) return;
        dialog->lifecycle.begin();
        dialog->busy = value;
        dialog->download->setText(
            value ? dialog->tr("Abort") : dialog->tr("Download"));
        dialog->download->setEnabled(true);
        dialog->close->setEnabled(true);
    }

    static void clickAbort(StravaRoutesDownload *dialog)
    {
        if (dialog) dialog->download->click();
    }

    static void clickClose(StravaRoutesDownload *dialog)
    {
        if (dialog) dialog->close->click();
    }

    static bool aborted(const StravaRoutesDownload *dialog)
    {
        return dialog && dialog->lifecycle.aborted();
    }

    static bool closeWhenIdle(const StravaRoutesDownload *dialog)
    {
        return dialog && dialog->lifecycle.closeWhenIdle();
    }

    static void attachOperation(
        StravaRoutesDownload *dialog,
        const ImportOperationPtr &operation)
    {
        if (!dialog || !operation || !operation->state) return;
        dialog->importOperation =
            std::make_shared<StravaRoutesDownload::ImportOperation>(
                operation->download, operation->state);
        for (const QString &routeId : operation->state->routeIds()) {
            QTreeWidgetItem *item = new QTreeWidgetItem(
                dialog->files->invisibleRootItem());
            item->setData(0, Qt::UserRole, routeId);
        }
    }

    static void finishDialogFinalization(
        StravaRoutesDownload *dialog,
        bool committed,
        bool stagingCleaned,
        const QString &error,
        bool operationFailed)
    {
        if (!dialog || !dialog->importOperation) return;
        const std::shared_ptr<StravaRoutesDownload::ImportOperation>
            operation = dialog->importOperation;
        dialog->finishFinalization(
            operation, committed, stagingCleaned,
            error, operationFailed);
    }
};

ImportOperationPtr prepareImport(
    const DownloadBatchResult &download,
    const QString &athleteRoot,
    const QString &workoutRoot,
    bool overwriteExisting,
    const GpxParserOptions &parserOptions,
    const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
    QString &error)
{
    error.clear();
    if (!download.isValid() || download.staged.isEmpty()) {
        error = QStringLiteral("The Strava route batch is unavailable.");
        return {};
    }
    auto state = std::make_shared<RouteImportState>(
        athleteRoot, workoutRoot, overwriteExisting, parserOptions);
    for (const StagedRoute &route : download.staged) {
        if (!state->prepare(route, cancelled, error)) return {};
    }
    if (!state->prepareJournal(error)) return {};
    return std::make_shared<ImportOperation>(download, std::move(state));
}

bool commitDecision(
    const ImportOperationPtr &operation,
    const QString &databasePath,
    const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
    bool &committed,
    QString &error)
{
    committed = false;
    if (!operation || !operation->state) {
        error = QStringLiteral("The Strava import operation is unavailable.");
        return false;
    }
    const auto databaseGeneration =
        TrainDB::captureDatabaseFileGeneration(databasePath, error);
    if (!databaseGeneration) return false;
    bool recoveryRequired = false;
    const bool result = operation->state->commitDecision(
        databasePath, databaseGeneration, cancelled,
        error, recoveryRequired);
    committed = recoveryRequired;
    return result;
}

bool publish(
    const ImportOperationPtr &operation,
    const StravaRoutesDownloadPipeline::CancellationCheck &cancelled,
    QString &error)
{
    if (!operation || !operation->state) {
        error = QStringLiteral("The Strava import operation is unavailable.");
        return false;
    }
    return operation->state->publish(cancelled, error);
}

bool finalize(
    const ImportOperationPtr &operation,
    bool &stagingCleaned,
    QString &error)
{
    stagingCleaned = false;
    if (!operation || !operation->state) {
        error = QStringLiteral("The Strava import operation is unavailable.");
        return false;
    }
    const bool committed = operation->state->commitDatabaseAtPath(error);
    stagingCleaned =
        StravaRoutesDownloadPipeline::removeStagingDirectory(
            operation->download);
    return committed;
}

bool finalizeWithDatabase(
    const ImportOperationPtr &operation,
    TrainDB *database,
    bool &stagingCleaned,
    QString &error)
{
    stagingCleaned = false;
    if (!operation || !operation->state || !database) {
        error = QStringLiteral("The Strava import operation is unavailable.");
        return false;
    }
    const bool committed = operation->state->commitDatabase(database, error);
    stagingCleaned =
        StravaRoutesDownloadPipeline::removeStagingDirectory(
            operation->download);
    return committed;
}

bool notifyFinalization(
    const ImportOperationPtr &operation,
    const std::function<void()> &notification)
{
    return StravaRoutesDownloadPipeline::notifyFinalizationWithLease(
        operation, notification);
}

StravaRoutesDownload *createDialog()
{
    return Access::createDialog();
}

void configureBusy(StravaRoutesDownload *dialog, bool busy)
{
    Access::configureBusy(dialog, busy);
}

void clickAbort(StravaRoutesDownload *dialog)
{
    Access::clickAbort(dialog);
}

void clickClose(StravaRoutesDownload *dialog)
{
    Access::clickClose(dialog);
}

bool aborted(const StravaRoutesDownload *dialog)
{
    return Access::aborted(dialog);
}

bool closeWhenIdle(const StravaRoutesDownload *dialog)
{
    return Access::closeWhenIdle(dialog);
}

void attachOperation(
    StravaRoutesDownload *dialog,
    const ImportOperationPtr &operation)
{
    Access::attachOperation(dialog, operation);
}

void finishDialogFinalization(
    StravaRoutesDownload *dialog,
    bool committed,
    bool stagingCleaned,
    const QString &error,
    bool operationFailed)
{
    Access::finishDialogFinalization(
        dialog, committed, stagingCleaned, error, operationFailed);
}

} // namespace StravaRoutesDownloadProductionTest
#endif

StravaRoutesDownload::StravaRoutesDownload(Context *context)
    : QDialog(context->mainWindow)
    , context(context)
    , busy(false)
    , overwriteExisting(false)
    , downloads(0)
    , fails(0)
    , activeRunner(nullptr)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setWindowTitle(tr("Download Routes as workouts from Strava"));

    HelpWhatsThis *help = new HelpWhatsThis(this);
    setWhatsThis(help->getWhatsThisText(
        HelpWhatsThis::MenuBar_Tools_Download_StravaRoutes));

    setMinimumWidth(650);
    setMinimumHeight(400);

    QVBoxLayout *layout = new QVBoxLayout;
    setLayout(layout);

    all = new QCheckBox(tr("check/uncheck all"), this);
    all->setChecked(true);
    refreshButton = new QPushButton(tr("Refresh List"), this);

    QHBoxLayout *topline = new QHBoxLayout;
    topline->addWidget(all);
    topline->addStretch();
    topline->addStretch();
    topline->addWidget(refreshButton);

    files = new QTreeWidget;
#ifdef Q_OS_MAC
    files->setAttribute(Qt::WA_MacShowFocusRect, 0);
#endif
    files->headerItem()->setText(0, tr(""));
    files->headerItem()->setText(1, tr("Name"));
    files->headerItem()->setText(2, tr("Description"));
    files->headerItem()->setText(3, tr("Action"));
    files->setColumnCount(4);
    files->setColumnWidth(0, 30 * dpiXFactor);
    files->setColumnWidth(1, 220 * dpiXFactor);
    files->setColumnWidth(2, 240 * dpiXFactor);
    files->setSelectionMode(QAbstractItemView::SingleSelection);
    files->setEditTriggers(QAbstractItemView::SelectedClicked);
    files->setUniformRowHeights(true);
    files->setIndentation(0);

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

    connectAbortAndCloseActions();
    connect(
        all, &QCheckBox::stateChanged,
        this, &StravaRoutesDownload::allClicked);
    connect(
        refreshButton, &QPushButton::clicked,
        this, &StravaRoutesDownload::refreshClicked);
    beginDownloadAction = [this] { beginSelectedDownloads(); };
    finishStagingAction = [this](
        const DownloadBatchResult &result,
        bool stagingCleaned,
        bool operationFailed) {
        finishStagingCleanup(
            result, stagingCleaned, operationFailed);
    };
    connect(
        context->mainWindow, &MainWindow::closingAthlete,
        this,
        [this](const QString &, Context *closing) {
            if (this->context
                && closing == this->context.data()) {
                contextClosing();
            }
        });
    connect(
        context, &QObject::destroyed,
        this,
        [this] { contextClosing(); });

    QTimer::singleShot(0, this, &StravaRoutesDownload::refreshClicked);
}

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
StravaRoutesDownload::StravaRoutesDownload(TestConstruction)
    : QDialog(nullptr)
    , context(nullptr)
    , busy(false)
    , overwriteExisting(false)
    , downloads(0)
    , fails(0)
    , activeRunner(nullptr)
{
    files = new QTreeWidget(this);
    all = new QCheckBox(this);
    overwrite = new QCheckBox(this);
    close = new QPushButton(tr("Close"), this);
    download = new QPushButton(tr("Download"), this);
    refreshButton = new QPushButton(tr("Refresh List"), this);
    status = new QLabel(this);
    beginDownloadAction = [] {};
    finishStagingAction = [](
        const DownloadBatchResult &, bool, bool) {
        stravaRoutesDownloadFinishStagingCleanupTestHook();
    };
    connectAbortAndCloseActions();
}
#endif

StravaRoutesDownload::~StravaRoutesDownload()
{
    if (activeRunner) {
        Runner *runner = activeRunner;
        activeRunner = nullptr;
        delete runner;
    }
}

void StravaRoutesDownload::connectAbortAndCloseActions()
{
    connect(
        download, &QPushButton::clicked,
        this, &StravaRoutesDownload::downloadClicked);
    connect(
        close, &QPushButton::clicked,
        this, &StravaRoutesDownload::cancelClicked);
}

void StravaRoutesDownload::contextClosing()
{
    lifecycle.requestAbort(true);
    importOperation.reset();
    if (activeRunner) {
        Runner *runner = activeRunner;
        activeRunner = nullptr;
        delete runner;
    }
    context = nullptr;
    reject();
}

Strava *StravaRoutesDownload::createStravaService() const
{
    if (!context) return nullptr;
    CloudService *configured =
        CloudServiceFactory::instance().newService(
            QStringLiteral("Strava"), context);
    Strava *service = dynamic_cast<Strava *>(configured);
    if (!service) delete configured;
    return service;
}

void StravaRoutesDownload::allClicked()
{
    const bool checked = all->isChecked();
    for (int index = 0;
         index < files->invisibleRootItem()->childCount();
         ++index) {
        QTreeWidgetItem *current =
            files->invisibleRootItem()->child(index);
        if (QCheckBox *box = static_cast<QCheckBox *>(
                files->itemWidget(current, 0))) {
            box->setChecked(checked);
        }
    }
}

void StravaRoutesDownload::downloadClicked()
{
    if (busy && download->text() == tr("Abort")) {
        lifecycle.requestAbort(false);
        download->setEnabled(false);
        status->setText(tr("Cancelling..."));
        if (activeRunner) activeRunner->cancel();
        return;
    }
    if (busy) return;

    if (beginDownloadAction) beginDownloadAction();
}

void StravaRoutesDownload::beginSelectedDownloads()
{

    downloads = fails = 0;
    lifecycle.begin();
    overwriteExisting = overwrite->isChecked();
    overwrite->setEnabled(false);
    pendingRouteIds.clear();
    for (int index = 0;
         index < files->invisibleRootItem()->childCount();
         ++index) {
        QTreeWidgetItem *current =
            files->invisibleRootItem()->child(index);
        QCheckBox *box = static_cast<QCheckBox *>(
            files->itemWidget(current, 0));
        if (!box || !box->isChecked()) continue;
        const QString routeId =
            current->data(0, Qt::UserRole).toString();
        if (!routeId.isEmpty()) {
            pendingRouteIds.append(routeId);
            current->setText(3, tr("Queued"));
        }
    }

    busy = true;
    refreshButton->setEnabled(false);
    status->setText(tr("Download..."));
    status->show();
    close->hide();
    download->setText(tr("Abort"));
    startDownloadBatch();
}

void StravaRoutesDownload::cancelClicked()
{
    if (!busy) {
        reject();
        return;
    }
    lifecycle.requestAbort(true);
    close->setEnabled(false);
    download->setEnabled(false);
    status->setText(tr("Cancelling..."));
    status->show();
    if (activeRunner) activeRunner->cancel();
}

void StravaRoutesDownload::refreshClicked()
{
    if (busy) return;

    lifecycle.begin();
    busy = true;
    status->clear();
    status->hide();
    downloads = fails = 0;
    download->setEnabled(false);
    download->show();
    close->show();
    close->setEnabled(true);
    refreshButton->setEnabled(false);
    files->clear();

    Strava *service = createStravaService();
    if (!service) {
        StravaRoutesClient::RoutesResult result;
        result.error = tr("The Strava service is unavailable.");
        finishRefresh(result, false);
        return;
    }

    auto result = std::make_shared<
        SharedResult<StravaRoutesClient::RoutesResult>>();
    const auto generation = lifecycle.generation();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [service, result](
            const StravaRoutesDownloadPipeline::CancellationCheck
                &cancelled) {
            StravaRoutesClient client(
                [service](
                    const QUrl &url,
                    qsizetype maximumBytes,
                    const StravaRoutesClient::CancellationCheck &check) {
                    return service->authenticatedGet(
                        url, maximumBytes, check);
                });
            result->store(client.listRoutes(cancelled));
        },
        [this, runner, result, generation](bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            if (!lifecycle.accepts(generation)) {
                StravaRoutesClient::RoutesResult cancelled;
                finishRefresh(cancelled, false);
                return;
            }
            finishRefresh(result->load(), operationFailed);
        },
        service);
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        delete service;
        StravaRoutesClient::RoutesResult failure;
        failure.error = tr("The Strava Routes worker could not be started.");
        finishRefresh(failure, true);
    }
}

void StravaRoutesDownload::finishRefresh(
    const StravaRoutesClient::RoutesResult &result,
    bool operationFailed)
{
    busy = false;
    download->setEnabled(true);
    refreshButton->setEnabled(true);
    close->setEnabled(true);
    if (lifecycle.closeWhenIdle()) {
        reject();
        return;
    }
    if (lifecycle.aborted()) return;
    if (operationFailed || !result.isValid()) {
        QMessageBox::warning(
            this,
            tr("Strava Route Download"),
            tr("The following error occurred: %1")
                .arg(result.error.isEmpty()
                    ? tr("The Strava Routes request failed.")
                    : result.error));
        return;
    }

    for (const StravaRouteSummary &item : result.routes) {
        QTreeWidgetItem *entry =
            new QTreeWidgetItem(files->invisibleRootItem());
        entry->setFlags(entry->flags() | Qt::ItemIsEditable);
        QCheckBox *checkBox = new QCheckBox("", this);
        checkBox->setChecked(true);
        files->setItemWidget(entry, 0, checkBox);
        entry->setText(1, item.name);
        entry->setText(2, item.description);
        entry->setText(3, tr("Download"));
        entry->setData(0, Qt::UserRole, item.routeId);
    }
}

void StravaRoutesDownload::startDownloadBatch()
{
    if (!context || lifecycle.aborted() || pendingRouteIds.isEmpty()) {
        finishDownload();
        return;
    }

    const int visibleBatch = qMin(
        pendingRouteIds.size(),
        StravaRoutesDownloadPipeline::maximumFilesPerBatch());
    for (int index = 0; index < visibleBatch; ++index) {
        setRouteStatus(
            pendingRouteIds.at(index), tr("Downloading..."));
    }

    Strava *service = createStravaService();
    if (!service) {
        DownloadBatchResult failure;
        failure.error = tr("The Strava service is unavailable.");
        finishDownloadBatch(failure, false);
        return;
    }

    const QStringList routeIds = pendingRouteIds;
    const QString stagingParent =
        context->athlete->home->temp().absolutePath();
    auto result = std::make_shared<
        SharedResult<DownloadBatchResult>>();
    const auto generation = lifecycle.generation();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [service, result, routeIds, stagingParent](
            const StravaRoutesDownloadPipeline::CancellationCheck
                &cancelled) {
            StravaRoutesClient client(
                [service](
                    const QUrl &url,
                    qsizetype maximumBytes,
                    const StravaRoutesClient::CancellationCheck &check) {
                    return service->authenticatedGet(
                        url, maximumBytes, check);
                });
            result->store(
                StravaRoutesDownloadPipeline::stageDownloadBatch(
                    routeIds,
                    stagingParent,
                    [&client](
                        const QString &routeId,
                        const StravaRoutesDownloadPipeline::
                            CancellationCheck &check) {
                        return client.downloadGpx(routeId, check);
                    },
                    cancelled));
        },
        [this, runner, result, generation](bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            const DownloadBatchResult completed = result->load();
            if (!lifecycle.accepts(generation)) {
                continueAfterImport(completed);
                return;
            }
            finishDownloadBatch(completed, operationFailed);
        },
        service);
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        delete service;
        DownloadBatchResult failure;
        failure.error = tr("The Strava Routes worker could not be started.");
        finishDownloadBatch(failure, true);
    }
}

void StravaRoutesDownload::finishDownloadBatch(
    const DownloadBatchResult &result,
    bool operationFailed)
{
    if (lifecycle.aborted() || lifecycle.closeWhenIdle() || !context) {
        continueAfterImport(result);
        return;
    }
    if (operationFailed || !result.isValid()) {
        const QString error = result.error.isEmpty()
            ? tr("The Strava route download failed.")
            : result.error;
        for (const QString &routeId : std::as_const(pendingRouteIds)) {
            setRouteStatus(routeId, tr("Error downloading"), error);
            ++fails;
        }
        pendingRouteIds.clear();
        finishDownload();
        return;
    }

    pendingRouteIds = result.remainingRouteIds;
    for (const RouteFailure &failure : result.failures) {
        setRouteStatus(
            failure.routeId, tr("Error downloading"), failure.error);
        ++fails;
    }

    if (!result.staged.isEmpty()) {
        startImportBatch(result);
        return;
    }

    continueAfterImport(result);
}

void StravaRoutesDownload::startImportBatch(
    const DownloadBatchResult &result)
{
    if (!context || !context->athlete
        || !context->athlete->home) {
        for (const StagedRoute &route : result.staged) {
            setRouteStatus(
                route.routeId, tr("Import failed"),
                tr("The workout library is unavailable."));
            ++fails;
        }
        continueAfterImport(result);
        return;
    }
    const QString workoutDirectory =
        appsettings->value(this, GC_WORKOUTDIR).toString();
    const QString athleteRoot =
        context->athlete->home->root().absolutePath();
    const GpxParserOptions parserOptions = GpxParser::captureOptions();
    importOperation = std::make_shared<ImportOperation>(
        result, athleteRoot, workoutDirectory, overwriteExisting,
        parserOptions);
    for (const StagedRoute &route : result.staged)
        setRouteStatus(route.routeId, tr("Validating..."));

    const std::shared_ptr<ImportOperation> operation = importOperation;
    const std::shared_ptr<RouteImportState> state = operation->state;
    const auto generation = lifecycle.generation();
    const QList<StagedRoute> routes = result.staged;
    auto preparation = std::make_shared<SharedResult<PreparationResult>>();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [operation, state, routes, preparation](
            const StravaRoutesDownloadPipeline::CancellationCheck
                &cancelled) {
            PreparationResult result;
            for (qsizetype index = 0; index < routes.size(); ++index) {
                if (cancelled()) {
                    for (qsizetype remaining = index;
                         remaining < routes.size(); ++remaining) {
                        result.cancelledRouteIds.append(
                            routes.at(remaining).routeId);
                    }
                    break;
                }
                QString error;
                if (!state->prepare(
                        routes.at(index), cancelled, error)) {
                    if (cancelled()) {
                        for (qsizetype remaining = index;
                             remaining < routes.size(); ++remaining) {
                            result.cancelledRouteIds.append(
                                routes.at(remaining).routeId);
                        }
                        break;
                    }
                    result.failures.append({
                        routes.at(index).routeId,
                        error.isEmpty()
                            ? QStringLiteral(
                                "The Strava route could not be prepared.")
                            : error
                    });
                }
            }
            if (!state->routeIds().isEmpty()
                && !state->prepareJournal(result.error)
                && result.error.isEmpty()) {
                result.error = QStringLiteral(
                    "The Strava import journal could not be prepared.");
            }
            preparation->store(std::move(result));
        },
        [this, runner, operation, preparation, generation](
            bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            if (!lifecycle.accepts(generation)
                || importOperation != operation) {
                if (importOperation == operation) importOperation.reset();
                continueAfterImport(operation->download);
                return;
            }
            const PreparationResult result = preparation->load();
            finishPreparationBatch(
                result.failures,
                result.cancelledRouteIds,
                result.error,
                operationFailed);
        });
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        finishPreparationBatch(
            {}, {},
            tr("The Strava route validation worker could not be started."),
            true);
    }
}

void StravaRoutesDownload::finishPreparationBatch(
    const QList<RouteFailure> &preparationFailures,
    const QStringList &cancelledRouteIds,
    const QString &preparationError,
    bool operationFailed)
{
    if (!importOperation) return;
    for (const RouteFailure &failure : preparationFailures) {
        setRouteStatus(
            failure.routeId, tr("Import failed"), failure.error);
        ++fails;
    }
    for (const QString &routeId : cancelledRouteIds)
        setRouteStatus(routeId, tr("Cancelled"));

    const QStringList preparedIds =
        importOperation->state->routeIds();
    if (operationFailed || !preparationError.isEmpty()) {
        const QString detail = preparationError.isEmpty()
            ? tr("The Strava route validation failed.")
            : preparationError;
        for (const QString &routeId : preparedIds) {
            setRouteStatus(routeId, tr("Import failed"), detail);
            ++fails;
        }
        const DownloadBatchResult result = importOperation->download;
        importOperation.reset();
        continueAfterImport(result);
        return;
    }
    if (preparedIds.isEmpty()) {
        const DownloadBatchResult result = importOperation->download;
        importOperation.reset();
        continueAfterImport(result);
        return;
    }
    for (const QString &routeId : preparedIds)
        setRouteStatus(routeId, tr("Prepared"));

    startDecisionCommit();
}

void StravaRoutesDownload::startDecisionCommit()
{
    if (!importOperation) return;
    const TrainDB::SchemaStatus schemaStatus = trainDB
        ? trainDB->schemaStatus()
        : TrainDB::SchemaStatus::uninitialized;
    if (!context || !trainDB
        || QThread::currentThread() != trainDB->thread()
        || (schemaStatus != TrainDB::SchemaStatus::current
            && schemaStatus != TrainDB::SchemaStatus::migrationReady)) {
        finishDecisionCommit(
            false, false,
            tr("The workout database is unavailable."), true);
        return;
    }
    const QString databasePath = trainDB->databaseFilePath();
    QString generationError;
    const auto databaseGeneration =
        TrainDB::captureDatabaseFileGeneration(
            databasePath, generationError);
    if (!databaseGeneration) {
        finishDecisionCommit(
            false, false,
            generationError.isEmpty()
                ? tr("The workout database generation is unavailable.")
                : generationError,
            true);
        return;
    }
    const std::shared_ptr<ImportOperation> operation = importOperation;
    const std::shared_ptr<RouteImportState> state = operation->state;
    const auto generation = lifecycle.generation();
    auto decision = std::make_shared<SharedResult<DecisionResult>>();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [operation, state, databasePath, databaseGeneration, decision](
            const StravaRoutesDownloadPipeline::CancellationCheck
                &cancelled) {
            DecisionResult result;
            result.committed = state->commitDecision(
                databasePath, databaseGeneration, cancelled, result.error,
                result.recoveryRequired);
            decision->store(std::move(result));
        },
        [this, runner, operation, decision, generation](
            bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            if (importOperation != operation) {
                continueAfterImport(operation->download);
                return;
            }
            const DecisionResult result = decision->load();
            if (!lifecycle.accepts(generation)) {
                finishDecisionCommit(
                    result.committed, result.recoveryRequired,
                    result.error, operationFailed);
                return;
            }
            finishDecisionCommit(
                result.committed, result.recoveryRequired,
                result.error, operationFailed);
        });
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        finishDecisionCommit(
            false, false,
            tr("The workout database worker could not be started."),
            true);
    }
}

void StravaRoutesDownload::finishDecisionCommit(
    bool committed,
    bool recoveryRequired,
    const QString &decisionError,
    bool operationFailed)
{
    if (!importOperation) return;
    const QStringList preparedIds =
        importOperation->state->routeIds();
    const bool continueImport = committed && !operationFailed
        && !lifecycle.aborted() && !lifecycle.closeWhenIdle() && context;
    if (!continueImport) {
        const bool requiresRecovery = recoveryRequired || committed;
        for (const QString &routeId : preparedIds) {
            setRouteStatus(
                routeId,
                requiresRecovery
                    ? tr("Recovery required")
                    : (lifecycle.aborted()
                        ? tr("Cancelled") : tr("Import failed")),
                decisionError.isEmpty()
                    ? (requiresRecovery
                        ? tr("The Strava route import requires recovery.")
                        : tr("The Strava route import failed."))
                    : decisionError);
            ++fails;
        }
        const DownloadBatchResult result = importOperation->download;
        importOperation.reset();
        continueAfterImport(result);
        return;
    }
    startPublication();
}

void StravaRoutesDownload::startPublication()
{
    if (!importOperation) return;
    const std::shared_ptr<ImportOperation> operation = importOperation;
    const std::shared_ptr<RouteImportState> state = operation->state;
    const auto generation = lifecycle.generation();
    auto publication = std::make_shared<SharedResult<PublicationResult>>();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [operation, state, publication](
            const StravaRoutesDownloadPipeline::CancellationCheck
                &cancelled) {
            PublicationResult result;
            result.published = state->publish(cancelled, result.error);
            publication->store(std::move(result));
        },
        [this, runner, operation, publication, generation](
            bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            if (importOperation != operation) return;
            const PublicationResult result = publication->load();
            if (!lifecycle.accepts(generation)
                && !result.published) {
                finishPublication(
                    false, result.error, operationFailed);
                return;
            }
            finishPublication(
                result.published, result.error, operationFailed);
        });
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        finishPublication(
            false,
            tr("The workout publication worker could not be started."),
            true);
    }
}

void StravaRoutesDownload::finishPublication(
    bool published,
    const QString &publicationError,
    bool operationFailed)
{
    if (!importOperation) return;
    if (published && !operationFailed) {
        startFinalization();
        return;
    }
    const std::shared_ptr<ImportOperation> operation = importOperation;
    const QStringList preparedIds = operation->state->routeIds();
    for (const QString &routeId : preparedIds) {
        setRouteStatus(
            routeId,
            tr("Recovery required"),
            publicationError.isEmpty()
                ? tr("The Strava route import requires recovery.")
                : publicationError);
        ++fails;
    }

    const DownloadBatchResult result = operation->download;
    if (importOperation == operation) importOperation.reset();
    continueAfterImport(result);
}

void StravaRoutesDownload::startFinalization()
{
    if (!importOperation) return;
    const std::shared_ptr<ImportOperation> operation = importOperation;
    auto finalization = std::make_shared<SharedResult<FinalizationResult>>();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [operation, finalization](
            const StravaRoutesDownloadPipeline::CancellationCheck &) {
            FinalizationResult result;
            result.committed =
                operation->state->commitDatabaseAtPath(result.error);
            result.stagingCleaned =
                StravaRoutesDownloadPipeline::removeStagingDirectory(
                    operation->download);
            finalization->store(std::move(result));
        },
        [this, runner, operation, finalization](bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            const FinalizationResult result = finalization->load();
            finishFinalization(
                operation, result.committed, result.stagingCleaned,
                result.error, operationFailed);
        });
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        finishFinalization(
            operation, false, false,
            tr("The workout finalization worker could not be started."),
            true);
    }
}

void StravaRoutesDownload::finishFinalization(
    std::shared_ptr<ImportOperation> operation,
    bool committed,
    bool stagingCleaned,
    const QString &finalizationError,
    bool operationFailed)
{
    if (!operation) return;
    QPointer<StravaRoutesDownload> guard(this);
    const auto continueFinalization = finishStagingAction;
    const QStringList preparedIds = operation->state->routeIds();
    const bool succeeded = committed && !operationFailed;
    for (const QString &routeId : preparedIds) {
        if (succeeded) {
            setRouteStatus(routeId, tr("Saved"));
            if (!guard) return;
            ++downloads;
        } else {
            setRouteStatus(
                routeId,
                tr("Recovery required"),
                finalizationError.isEmpty()
                    ? tr("The Strava route import requires recovery.")
                    : finalizationError);
            if (!guard) return;
            ++fails;
        }
    }

    const DownloadBatchResult result = operation->download;
    if (importOperation == operation) importOperation.reset();
    if (succeeded && trainDB) {
        StravaRoutesDownloadPipeline::notifyFinalizationWithLease(
            operation, [] {
                if (trainDB) trainDB->dataChanged();
            });
    }
    if (!guard) return;
    if (continueFinalization) {
        continueFinalization(result, stagingCleaned, operationFailed);
    }
}

void StravaRoutesDownload::continueAfterImport(
    const DownloadBatchResult &result)
{
    if (!result.stagingArea) {
        finishStagingCleanup(result, true, false);
        return;
    }
    auto cleaned = std::make_shared<SharedResult<bool>>();
    Runner *runner = new Runner(this);
    activeRunner = runner;
    const bool started = runner->start(
        [result, cleaned](
            const StravaRoutesDownloadPipeline::CancellationCheck &) {
            cleaned->store(
                StravaRoutesDownloadPipeline::removeStagingDirectory(
                    result));
        },
        [this, runner, result, cleaned](bool operationFailed) {
            if (activeRunner == runner) activeRunner = nullptr;
            runner->deleteLater();
            finishStagingCleanup(
                result, cleaned->load(), operationFailed);
        });
    if (!started) {
        activeRunner = nullptr;
        runner->deleteLater();
        finishStagingCleanup(result, false, true);
    }
}

void StravaRoutesDownload::finishStagingCleanup(
    const DownloadBatchResult &result,
    bool cleaned,
    bool operationFailed)
{
    if (!cleaned || operationFailed) {
        qWarning() << "Could not remove Strava route staging directory";
    }
    if (lifecycle.aborted() || result.cancelled) {
        for (const QString &routeId : std::as_const(pendingRouteIds))
            setRouteStatus(routeId, tr("Cancelled"));
        pendingRouteIds.clear();
        finishDownload();
        return;
    }
    if (pendingRouteIds.isEmpty()) {
        finishDownload();
        return;
    }
    QTimer::singleShot(
        0, this, &StravaRoutesDownload::startDownloadBatch);
}

void StravaRoutesDownload::finishDownload()
{
    status->setText(
        tr("%1 workouts downloaded, %2 failed or skipped.")
            .arg(downloads)
            .arg(fails));
    download->setText(tr("Download"));
    download->setEnabled(true);
    close->show();
    close->setEnabled(true);
    refreshButton->setEnabled(true);
    overwrite->setEnabled(true);
    busy = false;
    if (downloads > 0 && context) context->notifyWorkoutsChanged();
    if (lifecycle.closeWhenIdle()) reject();
}

QTreeWidgetItem *StravaRoutesDownload::itemForRoute(
    const QString &routeId) const
{
    for (int index = 0;
         index < files->invisibleRootItem()->childCount();
         ++index) {
        QTreeWidgetItem *item =
            files->invisibleRootItem()->child(index);
        if (item->data(0, Qt::UserRole).toString() == routeId)
            return item;
    }
    return nullptr;
}

void StravaRoutesDownload::setRouteStatus(
    const QString &routeId,
    const QString &text,
    const QString &toolTip)
{
    if (QTreeWidgetItem *item = itemForRoute(routeId)) {
        item->setText(3, text);
        item->setToolTip(3, toolTip);
        files->setCurrentItem(item);
    }
}

void StravaRoutesDownload::closeEvent(QCloseEvent *event)
{
    if (!busy) {
        QDialog::closeEvent(event);
        return;
    }

    lifecycle.requestAbort(true);
    close->setEnabled(false);
    download->setEnabled(false);
    status->setText(tr("Cancelling..."));
    status->show();
    if (activeRunner) activeRunner->cancel();
    event->ignore();
}
