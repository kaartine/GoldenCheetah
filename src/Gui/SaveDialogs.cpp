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

#include <exception>

#include "MainWindow.h"
#include "AthleteTab.h"
#include "Athlete.h"
#include "RideCache.h"
#include "Estimator.h"
#include "GcRideFile.h"
#include "JsonRideFile.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideFileCommand.h"
#include "Settings.h"
#include "SaveDialogs.h"
#include "DataProcessor.h"

//----------------------------------------------------------------------
// Utility functions to get and set WARN on CONVERT application setting
//----------------------------------------------------------------------
static bool
warnOnConvert()
{
    bool setting;

    QVariant warnsetting = appsettings->value(NULL, GC_WARNCONVERT);
    if (warnsetting.isNull()) setting = true;
    else setting = warnsetting.toBool();
    return setting;
}

void
setWarnOnConvert(bool setting)
{
    appsettings->setValue(GC_WARNCONVERT, setting);
}

static bool
warnExit()
{
    return appsettings->value(NULL, GC_WARNEXIT, true).toBool();
}

void
setWarnExit(bool setting)
{
    appsettings->setValue(GC_WARNEXIT, setting);
}

static bool
runDefaultSaveProcessors(RideFile *ride, QString &error)
{
    try {
        DataProcessorFactory::instance().autoProcess(
            ride, QStringLiteral("Save"), QStringLiteral("UPDATE"));
    } catch (const QString &detail) {
        error = detail;
        return false;
    } catch (const std::exception &exception) {
        error = QString::fromLocal8Bit(exception.what());
        return false;
    } catch (...) {
        error = QObject::tr("An activity processor failed");
        return false;
    }
    return true;
}

//----------------------------------------------------------------------
bool
saveActivityTransaction(Context *context, RideFile *ride,
                        const QString &targetPath,
                        const ActivitySaveOperations &operations,
                        QString &error)
{
    error.clear();
    if (!ride) {
        error = QObject::tr("Cannot open the activity for saving");
        return false;
    }
    if (!operations.writerFactory) {
        error = QObject::tr("Cannot create the atomic activity writer");
        return false;
    }
    if (!operations.finalize || !operations.markClean) {
        error = QObject::tr("Cannot complete the activity save");
        return false;
    }

    if (operations.stage && !operations.stage(ride, error)) {
        if (error.isEmpty()) {
            error = QObject::tr("An activity processor failed");
        }
        return false;
    }

    const QString historyKey = QStringLiteral("Change History");
    const bool hadHistory = ride->tags().contains(historyKey);
    const QString previousHistory =
        ride->getTag(historyKey, QString());
    QString history = previousHistory;
    const QDateTime timestamp = operations.timestamp.isValid()
        ? operations.timestamp
        : QDateTime::currentDateTime();
    history += QObject::tr("Changes on ");
    history += timestamp.toString() + QStringLiteral(":");
    history += QLatin1Char('\n') + ride->command->changeLog();
    ride->setTag(historyKey, history);

    JsonFileReader reader(operations.writerFactory);
    QFile targetFile(targetPath);
    const bool saved = completeActivitySave(
        [&](QString &stepError) {
            return reader.writeRideFile(context, ride, targetFile,
                                        stepError,
                                        operations.allowTargetReplacement,
                                        operations.targetLockHeld);
        },
        operations.finalize,
        operations.markClean,
        error,
        operations.rollback);

    if (!saved) {
        if (hadHistory) {
            ride->setTag(historyKey, previousHistory);
        } else {
            ride->removeTag(historyKey);
        }
    }
    return saved;
}

bool
saveActivityCandidate(RideItem *current, RideItem *candidate,
                      RideFile *replacement,
                      const ActivityCandidateSave &save,
                      QString &error)
{
    error.clear();
    if (!current || !candidate || current == candidate
        || !replacement || !save
        || candidate->ride(false) != replacement) {
        error = QObject::tr("Cannot prepare the replacement activity");
        return false;
    }

    candidate->path = current->path;
    candidate->fileName = current->fileName;
    candidate->setDirty(true);

    // The candidate is not in RideCache and must not publish saved signals.
    QObject::disconnect(replacement, nullptr, candidate, nullptr);
    if (!save(candidate, error)) {
        candidate->setRide(nullptr);
        candidate->setRide(replacement);
        if (error.isEmpty()) {
            error = QObject::tr("Cannot save the replacement activity");
        }
        return false;
    }

    const QString committedPath = candidate->path;
    const QString committedFileName = candidate->fileName;
    candidate->setRide(nullptr);

    current->setRide(replacement);
    current->setFileName(committedPath, committedFileName);
    current->saved();
    return true;
}

// User selected Save... menu option, prompt if conversion is needed
//----------------------------------------------------------------------
bool
MainWindow::saveRideSingleDialog(
    Context *context, RideItem *rideItem,
    const SaveRideDialogOperations *operations)
{
    if (!rideItem) {
        return false;
    }

    const QFileInfo currentFile(
        QDir(rideItem->path).filePath(rideItem->fileName));
    const QString currentType = currentFile.completeSuffix().toUpper();
    MainWindow *parent = context ? context->mainWindow : nullptr;

    if (currentType != QStringLiteral("GC") && warnOnConvert()) {
        SaveSingleDialogWidget dialog(parent, context, rideItem);
        dialog.exec();
        return dialog.mayProceed();
    }

    QString error;
    QList<RideItem *> activities;
    activities << rideItem;
    if (context) {
        relinkRideItems(context, rideItem, activities);
    }

    bool saved = false;
    if (operations && operations->saveActivities) {
        saved = operations->saveActivities(activities, error);
    } else if (context && context->athlete
               && context->athlete->rideCache) {
        RideCache *cache = context->athlete->rideCache;
        saved = RideCache::saveActivities(
            context, activities, error,
            [](Context *saveContext, RideItem *saveItem,
               QString *saveError) {
                return MainWindow::saveSilent(
                    saveContext, saveItem, saveError);
            },
            [cache](RideItem *savedItem) {
                QMetaObject::invokeMethod(
                    cache, "itemSaved", Qt::DirectConnection,
                    Q_ARG(RideItem *, savedItem));
            });
    } else {
        error = QObject::tr("Cannot access the activity collection");
    }

    if (!saved) {
        if (operations && operations->reportError) {
            operations->reportError(error);
        } else {
            QMessageBox::warning(
                parent, QObject::tr("Save Activity"), error);
        }
    }
    return saved;
}

//----------------------------------------------------------------------
// Check if data needs saving on exit and prompt user for action
//----------------------------------------------------------------------
bool
MainWindow::saveRideExitDialog(Context *context)
{
    QList<RideItem*> dirtyList;

    // have we been told to not warn on exit?
    if (warnExit() == false) return true; // just close regardless!

    // get a list of rides to save
    foreach (RideItem *rideItem, context->athlete->rideCache->rides())
        if (rideItem->isDirty() == true) 
            dirtyList.append(rideItem);

    // we have some files to save...
    if (dirtyList.count() > 0) {
        SaveOnExitDialogWidget dialog(this, context, dirtyList);
        QGuiApplication::setOverrideCursor(Qt::ArrowCursor);
        int result = dialog.exec();
        QGuiApplication::restoreOverrideCursor();
        if (result == QDialog::Rejected) return false; // cancel that closeEvent!
    }

    // You can exit and close now
    return true;
}

//----------------------------------------------------------------------
// Silently save ride and convert to GC format without warning user
//----------------------------------------------------------------------
bool
MainWindow::saveSilent(Context *context, RideItem *rideItem, QString *error,
                       const ActivitySaveOperations *requestedOperations)
{
    QString ignoredError;
    QString &saveError = error ? *error : ignoredError;
    saveError.clear();

    if (!rideItem) {
        saveError = QObject::tr("Cannot open the activity for saving");
        return false;
    }

    RideFile *ride = rideItem->ride();
    if (!ride) {
        saveError = QObject::tr("Cannot open the activity for saving");
        return false;
    }

    const QString sourcePath =
        QFileInfo(QDir(rideItem->path).filePath(rideItem->fileName))
            .absoluteFilePath();
    const QFileInfo sourceInfo(sourcePath);
    const bool convert = sourceInfo.completeSuffix().compare(
                             QStringLiteral("json"), Qt::CaseInsensitive) != 0;

    const QDateTime rideDateTime = ride->startTime();
    const QChar zero = QLatin1Char('0');
    const QString datedBaseName = QStringLiteral("%1_%2_%3_%4_%5_%6")
        .arg(rideDateTime.date().year(), 4, 10, zero)
        .arg(rideDateTime.date().month(), 2, 10, zero)
        .arg(rideDateTime.date().day(), 2, 10, zero)
        .arg(rideDateTime.time().hour(), 2, 10, zero)
        .arg(rideDateTime.time().minute(), 2, 10, zero)
        .arg(rideDateTime.time().second(), 2, 10, zero);

    const bool keepCurrentPath =
        !convert && sourceInfo.baseName() == datedBaseName;
    const QString targetPath = keepCurrentPath
        ? sourcePath
        : QDir(sourceInfo.absolutePath()).filePath(
              datedBaseName + QStringLiteral(".json"));
    const bool pathChanges =
        QDir::cleanPath(sourcePath) != QDir::cleanPath(targetPath);

    AtomicFileLockSet transactionLocks;
    if (!transactionLocks.lock(
            { sourcePath, targetPath }, saveError)) {
        return false;
    }

    AtomicFileSnapshot sourceSnapshot;
    if (pathChanges && !captureAtomicFileSnapshot(
            sourcePath, sourceSnapshot, saveError)) {
        return false;
    }

    if (pathChanges && QFile::exists(targetPath)) {
        saveError = QObject::tr(
                        "Cannot save activity because the target already exists: %1")
                        .arg(QFileInfo(targetPath).fileName());
        return false;
    }

    ActivitySaveOperations saveOperations;
    if (requestedOperations) {
        saveOperations = *requestedOperations;
    } else {
        saveOperations.writerFactory = qSaveFileWriterFactory();
        saveOperations.stage = [](RideFile *activity, QString &stageError) {
            return runDefaultSaveProcessors(activity, stageError);
        };
    }
    saveOperations.allowTargetReplacement = !pathChanges;
    saveOperations.targetLockHeld = true;
    if (!saveOperations.timestamp.isValid()) {
        saveOperations.timestamp = QDateTime::currentDateTime();
    }

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    saveOperations.finalize = [&](QString &stepError) {
        if (pathChanges && !atomicFileMatchesSnapshot(
                sourcePath, sourceSnapshot, stepError)) {
            return false;
        }
        if (!finalizeActivityFileReplacement(sourcePath, targetPath,
                                             convert, stepError)) {
            return false;
        }
        if (pathChanges) {
            rideItem->setFileName(sourceInfo.absolutePath(),
                                  QFileInfo(targetPath).fileName());
        }
        return true;
    };
    if (pathChanges) {
        saveOperations.rollback = [&](QString &rollbackError) {
            const QFileInfo target(targetPath);
            if (!target.exists() && !target.isSymLink()) {
                return true;
            }
            if (!QFile::remove(targetPath)) {
                rollbackError = QObject::tr(
                    "Cannot remove the unfinalized activity");
                return false;
            }
            return syncParentDirectory(targetPath, rollbackError);
        };
    }
    saveOperations.markClean = [&]() { ride->emitSaved(); };

    const bool saved = saveActivityTransaction(
        context, ride, targetPath, saveOperations, saveError);

    if (saved) {
        QFile notesFile(QDir(sourceInfo.absolutePath()).filePath(
            sourceInfo.baseName() + QStringLiteral(".notes")));
        if (notesFile.exists()) {
            notesFile.remove();
        }
        if (context && context->athlete
            && context->athlete->rideCache
            && context->athlete->rideCache->estimator) {
            context->athlete->rideCache->estimator->refresh();
        }
    }

    QGuiApplication::restoreOverrideCursor();
    return saved;
}


void
MainWindow::saveAllFilesSilent(Context *context)
{
    // iterate over snapshot of rides to prevent crash by iterator invalidation
    const QList<RideItem*> snapshot = context->athlete->rideCache->rides().toList();
    for (RideItem *rideItem : snapshot) {
        if (rideItem->isDirty()) {
            this->saveRideSingleDialog(context, rideItem);
        }
    }
}

//----------------------------------------------------------------------
// Save Single File Dialog Widget
//----------------------------------------------------------------------
SaveSingleDialogWidget::SaveSingleDialogWidget(MainWindow *mainWindow, Context *context, RideItem *rideItem) :
    QDialog(mainWindow, Qt::Dialog), mainWindow(mainWindow), context(context), rideItem(rideItem)
{
    setWindowTitle(tr("Save and Conversion"));
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Warning text
    warnText = new QLabel(tr("WARNING\n\nYou have made changes to ") + rideItem->fileName + tr(" If you want to save\nthem, we need to convert to GoldenCheetah\'s\nnative format. Should we do so?\n"));
    mainLayout->addWidget(warnText);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout;
    saveButton = new QPushButton(tr("&Save and Convert"), this);
    buttonLayout->addWidget(saveButton);
    abandonButton = new QPushButton(tr("&Discard Changes"), this);
    buttonLayout->addWidget(abandonButton);
    cancelButton = new QPushButton(tr("&Cancel Save"), this);
    buttonLayout->addWidget(cancelButton);
    mainLayout->addLayout(buttonLayout);

    // Don't warn me!
    warnCheckBox = new QCheckBox(tr("Always warn me about file conversions"), this);
    warnCheckBox->setChecked(true);
    mainLayout->addWidget(warnCheckBox);

    // connect up slots
    connect(saveButton, SIGNAL(clicked()), this, SLOT(saveClicked()));
    connect(abandonButton, SIGNAL(clicked()), this, SLOT(abandonClicked()));
    connect(cancelButton, SIGNAL(clicked()), this, SLOT(cancelClicked()));
    connect(warnCheckBox, SIGNAL(clicked()), this, SLOT(warnSettingClicked()));
}

bool
SaveSingleDialogWidget::saveRide(QString &error)
{
    return MainWindow::saveSilent(context, rideItem, &error);
}

void
SaveSingleDialogWidget::reportSaveError(const QString &error)
{
    QMessageBox::warning(this, tr("Save Activity"), error);
}

void
SaveSingleDialogWidget::saveClicked()
{
    QString error;
    if (!saveRide(error)) {
        reportSaveError(error);
        return;
    }
    mayProceed_ = true;
    accept();
}

void
SaveSingleDialogWidget::abandonClicked()
{
    rideItem->setDirty(false); // lose changes
    mayProceed_ = true;
    reject();
}

void
SaveSingleDialogWidget::cancelClicked()
{
    reject();
}

void
SaveSingleDialogWidget::warnSettingClicked()
{
    setWarnOnConvert(warnCheckBox->isChecked());
}

//----------------------------------------------------------------------
// Save on Exit File Dialog Widget
//----------------------------------------------------------------------

SaveOnExitDialogWidget::SaveOnExitDialogWidget(MainWindow *mainWindow, Context *context, QList<RideItem *>dirtyList) :
    QDialog(mainWindow, Qt::Dialog), mainWindow(mainWindow), context(context), dirtyList(dirtyList)
{
    setWindowTitle(tr("Save Changes"));
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Warning text
    const QString athleteName =
        context && context->athlete ? context->athlete->cyclist : QString();
    warnText = new QLabel(
        tr("WARNING for athlete %1\n\nYou have made changes to some rides which\n"
           "have not been saved. They are listed below.")
            .arg(athleteName));
    mainLayout->addWidget(warnText);

    // File List
    dirtyFiles = new QTableWidget(dirtyList.count(), 0, this);
    dirtyFiles->setColumnCount(2);
    dirtyFiles->horizontalHeader()->hide();
    dirtyFiles->verticalHeader()->hide();

    // Populate with dirty List
    for (int i=0; i<dirtyList.count(); i++) {
        // checkbox
        QCheckBox *c = new QCheckBox;
        c->setCheckState(Qt::Checked);
        dirtyFiles->setCellWidget(i,0,c);

        // filename
        QTableWidgetItem *t = new QTableWidgetItem;
        t->setText(dirtyList.at(i)->fileName);
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        dirtyFiles->setItem(i,1,t);
    }

    // prettify the list
    dirtyFiles->setShowGrid(false);
    dirtyFiles->resizeColumnToContents(0);
    dirtyFiles->resizeColumnToContents(1);
    mainLayout->addWidget(dirtyFiles);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout;
    saveButton = new QPushButton(tr("&Save and Exit"), this);
    buttonLayout->addWidget(saveButton);
    abandonButton = new QPushButton(tr("&Discard and Exit"), this);
    buttonLayout->addWidget(abandonButton);
    cancelButton = new QPushButton(tr("&Cancel Exit"), this);
    buttonLayout->addWidget(cancelButton);
    mainLayout->addLayout(buttonLayout);

    // Don't warn me!
    exitWarnCheckBox = new QCheckBox(tr("Always check for unsaved changes on exit"), this);
    exitWarnCheckBox->setChecked(true);
    mainLayout->addWidget(exitWarnCheckBox);

    // connect up slots
    connect(saveButton, SIGNAL(clicked()), this, SLOT(saveClicked()));
    connect(abandonButton, SIGNAL(clicked()), this, SLOT(abandonClicked()));
    connect(cancelButton, SIGNAL(clicked()), this, SLOT(cancelClicked()));
    connect(exitWarnCheckBox, SIGNAL(clicked()), this, SLOT(warnSettingClicked()));
}

bool
SaveOnExitDialogWidget::saveRide(RideItem *rideItem)
{
    return MainWindow::saveRideSingleDialog(context, rideItem);
}

void
SaveOnExitDialogWidget::saveClicked()
{
    QList<RideItem *> skippedItems;
    for (int i = 0; i < dirtyList.count(); ++i) {
        QCheckBox *checkBox =
            qobject_cast<QCheckBox *>(dirtyFiles->cellWidget(i, 0));
        if (checkBox && checkBox->isChecked()) {
            if (!dirtyList.at(i)->isDirty()) {
                continue;
            }
            if (!saveRide(dirtyList.at(i))) {
                return;
            }
        } else {
            skippedItems.append(dirtyList.at(i));
        }
    }

    for (RideItem *rideItem : skippedItems) {
        rideItem->skipsave = true;
    }
    accept();
}

void
SaveOnExitDialogWidget::abandonClicked()
{
    // we need to ensure the ride is refreshed when we restart
    // so mark the ride item as nosave to ensure rebuild
    for (int i=0; i<dirtyList.count(); i++) 
        dirtyList.at(i)->skipsave = true;

    accept();
}

void
SaveOnExitDialogWidget::cancelClicked()
{
    reject();
}

void
SaveOnExitDialogWidget::warnSettingClicked()
{
    setWarnExit(exitWarnCheckBox->isChecked());
}


bool
proceedDialog
(Context *context, const RideCache::OperationPreCheck &check)
{
    if (check.requiresUserDecision) {
        QMessageBox msgBox(QMessageBox::Question,
                           QObject::tr("Modified activities"),
                           check.warningMessage,
                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                           context->mainWindow);
        int action = msgBox.exec();
        if (action == QMessageBox::Cancel) {
            return false;
        } else if (action == QMessageBox::Save) {
            QString error;
            if (!context->athlete->rideCache->saveActivities(check.dirtyItems,
                                                              error)) {
                QMessageBox::warning(context->mainWindow,
                                     QObject::tr("Save Activity"), error);
                return false;
            }
        } else if (action == QMessageBox::Discard) {
            for (RideItem *item : check.dirtyItems) {
                item->close();
                item->ride();
            }
        }
    }
    return true;
}


void
relinkRideItems
(Context *context, RideItem *rideItem, QList<RideItem*> &activities)
{
    QString newFilename;
    bool hasNewFilename = context->mainWindow->filenameWillChange(rideItem, &newFilename);
    RideItem *linkedItem = context->athlete->rideCache->getLinkedActivity(rideItem);
    if (linkedItem != nullptr) {
        QString linkedNewFilename;
        bool hasLinkedNewFilename = context->mainWindow->filenameWillChange(linkedItem, &linkedNewFilename);
        if (hasNewFilename) {
            linkedItem->setLinkedFileName(newFilename);
            linkedItem->setDirty(true);
            activities << linkedItem;
        }
        if (hasLinkedNewFilename) {
            rideItem->setLinkedFileName(linkedNewFilename);
            rideItem->setDirty(true);
        }
    }
}
