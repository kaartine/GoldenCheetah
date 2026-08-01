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

#include <algorithm>
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

static ActivitySaveWorkflow::Identity
activitySaveIdentity(const RideItem *item)
{
    return item
        ? ActivitySaveWorkflow::Identity{
              item->fileName, item->path, item->planned}
        : ActivitySaveWorkflow::Identity{};
}

static bool
activitySaveCacheContainsUniqueIdentity(
    RideCache *cache,
    RideItem *item,
    const ActivitySaveWorkflow::Identity &identity)
{
    if (!cache || !item) return false;

    int matches = 0;
    for (RideItem *candidate : cache->rides()) {
        const ActivitySaveWorkflow::Identity current =
            activitySaveIdentity(candidate);
        if (current.fileName == identity.fileName
            && current.path == identity.path
            && current.planned == identity.planned) {
            ++matches;
            if (candidate != item) return false;
        }
    }
    return matches == 1;
}

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
    const bool contextRequired = context != nullptr;
    QPointer<Context> guardedContext(context);
    QPointer<RideFile> guardedRide(ride);
    const auto transactionIsCurrent = [&] {
        return guardedRide
            && (!contextRequired || guardedContext);
    };
    if (!transactionIsCurrent()) {
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

    if (operations.stage) {
        const bool staged = operations.stage(
            guardedRide.data(), error);
        if (!transactionIsCurrent()) {
            error = QObject::tr(
                "The activity changed while it was being prepared for saving");
            return false;
        }
        if (!staged) {
            if (error.isEmpty()) {
                error = QObject::tr("An activity processor failed");
            }
            return false;
        }
    }

    const QString historyKey = QStringLiteral("Change History");
    const bool hadHistory =
        guardedRide->tags().contains(historyKey);
    const QString previousHistory =
        guardedRide->getTag(historyKey, QString());
    QString history = previousHistory;
    const QDateTime timestamp = operations.timestamp.isValid()
        ? operations.timestamp
        : QDateTime::currentDateTime();
    history += QObject::tr("Changes on ");
    history += timestamp.toString() + QStringLiteral(":");
    history += QLatin1Char('\n')
        + guardedRide->command->changeLog();
    guardedRide->setTag(historyKey, history);

    JsonFileReader reader(operations.writerFactory);
    QFile targetFile(targetPath);
    bool ownerLostAfterPersist = false;
    const ActivitySaveStep guardedFinalize =
        [&](QString &stepError) {
            if (ownerLostAfterPersist) {
                if (operations.persistCompletesDurableTransaction) {
                    return true;
                }
                stepError = QObject::tr(
                    "The activity disappeared before the persisted file could be finalized");
                return false;
            }
            if (!transactionIsCurrent()) {
                stepError = QObject::tr(
                    "The activity changed before the save could be finalized");
                return false;
            }
            return operations.finalize(stepError);
        };
    const std::function<void()> guardedMarkClean = [&] {
        if (ownerLostAfterPersist) return;
        if (!transactionIsCurrent()) return;
        operations.markClean();
    };
    const bool saved = completeActivitySave(
        [&](QString &stepError) {
            if (!transactionIsCurrent()) {
                stepError = QObject::tr(
                    "The activity changed before it could be persisted");
                return false;
            }
            const bool persisted = reader.writeRideFile(
                guardedContext.data(), guardedRide.data(),
                targetFile, stepError,
                operations.allowTargetReplacement,
                operations.targetLockHeld);
            if (!transactionIsCurrent()) {
                if (persisted) {
                    ownerLostAfterPersist = true;
                    stepError.clear();
                    return true;
                }
                if (stepError.isEmpty()) {
                    stepError = QObject::tr(
                        "The activity changed while it was being persisted");
                }
                return false;
            }
            return persisted;
        },
        guardedFinalize,
        guardedMarkClean,
        error,
        operations.rollback);

    if (!saved && guardedRide) {
        if (hadHistory) {
            guardedRide->setTag(
                historyKey, previousHistory);
        } else {
            guardedRide->removeTag(historyKey);
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
    QPointer<RideItem> guardedCurrent(current);
    QPointer<RideItem> guardedCandidate(candidate);
    QPointer<RideFile> guardedReplacement(replacement);
    QPointer<RideFile> guardedOriginal(
        current ? current->ride(false) : nullptr);
    if (!current || !candidate || current == candidate
        || !replacement || !save
        || !guardedOriginal
        || guardedOriginal == guardedReplacement
        || candidate->ride(false) != replacement) {
        error = QObject::tr("Cannot prepare the replacement activity");
        return false;
    }

    const ActivitySaveWorkflow::Identity currentIdentity =
        activitySaveIdentity(guardedCurrent.data());
    const auto currentIsExpected = [&] {
        return guardedCurrent && guardedOriginal
            && guardedCurrent->ride(false)
                == guardedOriginal.data()
            && guardedCurrent->fileName
                == currentIdentity.fileName
            && guardedCurrent->path == currentIdentity.path
            && guardedCurrent->planned
                == currentIdentity.planned;
    };
    const auto candidateIsExpected =
        [&](bool savedIdentityAllowed) {
            return guardedCandidate && guardedReplacement
                && guardedCandidate->ride(false)
                    == guardedReplacement.data()
                && guardedCandidate->planned
                    == currentIdentity.planned
                && (savedIdentityAllowed
                    ? (!guardedCandidate->fileName.isEmpty()
                        && !guardedCandidate->path.isEmpty())
                    : guardedCandidate->fileName
                            == currentIdentity.fileName
                        && guardedCandidate->path
                            == currentIdentity.path);
        };
    const auto rejectChangedActivity = [&] {
        if (error.isEmpty()) {
            error = QObject::tr(
                "The replacement activity changed while it was being saved");
        }
        return false;
    };

    guardedCandidate->path = currentIdentity.path;
    guardedCandidate->fileName = currentIdentity.fileName;
    guardedCandidate->planned = currentIdentity.planned;
    guardedCandidate->setDirty(true);
    if (!currentIsExpected()
        || !candidateIsExpected(false)) {
        return rejectChangedActivity();
    }

    // The candidate is not in RideCache and must not publish saved signals.
    QObject::disconnect(
        guardedReplacement.data(), &RideFile::modified,
        guardedCandidate.data(), &RideItem::modified);
    QObject::disconnect(
        guardedReplacement.data(), &RideFile::saved,
        guardedCandidate.data(), &RideItem::saved);
    QObject::disconnect(
        guardedReplacement.data(), &RideFile::reverted,
        guardedCandidate.data(), &RideItem::reverted);
    const bool saved = save(guardedCandidate.data(), error);
    if (!saved) {
        if (guardedCandidate && guardedReplacement
            && guardedCandidate->ride(false)
                == guardedReplacement.data()) {
            guardedCandidate->setRide(nullptr);
            if (guardedCandidate && guardedReplacement) {
                guardedCandidate->setRide(
                    guardedReplacement.data());
            }
        }
        if (error.isEmpty()) {
            error = QObject::tr("Cannot save the replacement activity");
        }
        return false;
    }
    if (!currentIsExpected()
        || !candidateIsExpected(true)) {
        return rejectChangedActivity();
    }

    const QString committedPath = guardedCandidate->path;
    const QString committedFileName =
        guardedCandidate->fileName;
    guardedCandidate->setRide(nullptr);
    if (!guardedCandidate || !guardedReplacement
        || !currentIsExpected()
        || guardedCandidate->ride(false) != nullptr) {
        return rejectChangedActivity();
    }

    guardedCurrent->setRide(guardedReplacement.data());
    if (!guardedCurrent || !guardedReplacement
        || guardedCurrent->ride(false)
            != guardedReplacement.data()
        || guardedCurrent->fileName
            != currentIdentity.fileName
        || guardedCurrent->path != currentIdentity.path
        || guardedCurrent->planned
            != currentIdentity.planned) {
        return rejectChangedActivity();
    }
    guardedCurrent->setFileName(
        committedPath, committedFileName);
    if (!guardedCurrent || !guardedReplacement
        || guardedCurrent->ride(false)
            != guardedReplacement.data()
        || guardedCurrent->path != committedPath
        || guardedCurrent->fileName != committedFileName
        || guardedCurrent->planned
            != currentIdentity.planned) {
        return rejectChangedActivity();
    }
    guardedCurrent->saved();
    if (guardedCurrent
        && (!guardedReplacement
            || guardedCurrent->ride(false)
                != guardedReplacement.data()
            || guardedCurrent->path != committedPath
            || guardedCurrent->fileName
                != committedFileName
            || guardedCurrent->planned
                != currentIdentity.planned)) {
        return rejectChangedActivity();
    }
    return true;
}

// User selected Save... menu option, prompt if conversion is needed
//----------------------------------------------------------------------
bool
MainWindow::saveRideSingleDialog(
    Context *context, RideItem *rideItem,
    const SaveRideDialogOperations *operations)
{
    QPointer<Context> guardedContext(context);
    QPointer<RideItem> guardedRideItem(rideItem);
    QPointer<Athlete> guardedAthlete(
        guardedContext
            ? guardedContext->athlete
            : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete
            ? guardedAthlete->rideCache
            : nullptr);
    QPointer<MainWindow> parent(
        guardedContext
            ? guardedContext->mainWindow
            : nullptr);
    const bool contextRequired = context != nullptr;
    const auto collectionAvailable =
        [&](QString &validationError) {
            if (!contextRequired) return true;
            if (!guardedContext || !guardedAthlete
                || !guardedCache
                || guardedContext->athlete
                    != guardedAthlete.data()
                || guardedAthlete->rideCache
                    != guardedCache.data()) {
                if (validationError.isEmpty()) {
                    validationError = QObject::tr(
                        "The activity collection is no longer available");
                }
                return false;
            }
            return true;
        };

    if (!guardedRideItem) {
        return false;
    }

    const QFileInfo currentFile(
        QDir(guardedRideItem->path).filePath(
            guardedRideItem->fileName));
    const QString currentType = currentFile.completeSuffix().toUpper();

    if (currentType != QStringLiteral("GC") && warnOnConvert()) {
        QString validationError;
        if (!collectionAvailable(validationError)) {
            if (operations && operations->reportError) {
                operations->reportError(validationError);
            }
            return false;
        }
        QPointer<SaveSingleDialogWidget> dialog(
            new SaveSingleDialogWidget(
                parent.data(), guardedContext.data(),
                guardedRideItem.data()));
        dialog->exec();
        const bool mayProceed = dialog
            && dialog->mayProceed();
        if (dialog) delete dialog.data();
        return mayProceed;
    }

    QString error;
    GuardedOperationPreflightItems guardedActivities =
        guardOperationPreflightItems(
            QList<RideItem*>{guardedRideItem.data()});
    OperationPreflightSave save;
    if (operations && operations->saveActivities) {
        save = operations->saveActivities;
    } else if (contextRequired) {
        save = [guardedContext, guardedCache](
                   const QList<RideItem*> &items,
                   QString &saveError) {
            if (!guardedContext || !guardedCache) {
                saveError = QObject::tr(
                    "The activity collection is no longer available");
                return false;
            }
            return RideCache::saveActivities(
                guardedContext.data(), items, saveError,
                [](Context *saveContext, RideItem *saveItem,
                   QString *itemError) {
                    return MainWindow::saveSilent(
                        saveContext, saveItem, itemError);
                },
                [guardedCache](RideItem *savedItem) {
                    if (!guardedCache) return;
                    QMetaObject::invokeMethod(
                        guardedCache.data(), "itemSaved",
                        Qt::DirectConnection,
                        Q_ARG(RideItem *, savedItem));
                });
        };
    }

    bool saved = false;
    if (!save) {
        error = QObject::tr("Cannot access the activity collection");
    } else if (contextRequired) {
        saved = saveOperationPreflightActivities(
            guardedActivities,
            [guardedContext](
                RideItem *item,
                GuardedOperationPreflightItems &activities,
                QString &relinkError) {
                if (!guardedContext) {
                    relinkError = QObject::tr(
                        "The activity collection is no longer available");
                    return false;
                }
                return relinkRideItems(
                    guardedContext.data(), item,
                    activities, relinkError);
            },
            save, error, collectionAvailable);
    } else {
        QList<RideItem *> activities;
        if (resolveOperationPreflightItems(
                guardedActivities, activities, error)) {
            saved = save(activities, error);
        }
    }

    if (!saved) {
        if (operations && operations->reportError) {
            operations->reportError(error);
        } else if (parent || !contextRequired) {
            QMessageBox::warning(
                parent.data(), QObject::tr("Save Activity"), error);
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

    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete ? guardedAthlete->rideCache : nullptr);
    if (!guardedContext || !guardedAthlete || !guardedCache
        || guardedContext->athlete != guardedAthlete.data()
        || guardedAthlete->rideCache != guardedCache.data()) {
        return false;
    }

    // get a list of rides to save
    foreach (RideItem *rideItem, guardedCache->rides())
        if (rideItem->isDirty() == true) 
            dirtyList.append(rideItem);

    // we have some files to save...
    if (dirtyList.count() > 0) {
        QPointer<SaveOnExitDialogWidget> dialog(
            new SaveOnExitDialogWidget(
                this, guardedContext.data(), dirtyList));
        QGuiApplication::setOverrideCursor(Qt::ArrowCursor);
        const int result = dialog->exec();
        QGuiApplication::restoreOverrideCursor();
        if (dialog) delete dialog.data();
        if (result == QDialog::Rejected
            || !guardedContext || !guardedAthlete
            || !guardedCache
            || guardedContext->athlete
                != guardedAthlete.data()
            || guardedAthlete->rideCache
                != guardedCache.data()) {
            return false; // cancel that closeEvent!
        }
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

    const bool contextRequired = context != nullptr;
    QPointer<Context> guardedContext(context);
    QPointer<RideItem> guardedItem(rideItem);
    QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete ? guardedAthlete->rideCache : nullptr);
    ActivitySaveWorkflow::Identity expectedIdentity =
        activitySaveIdentity(guardedItem.data());
    const bool membershipRequired = guardedCache
        && guardedItem
        && guardedCache->rides().contains(
            guardedItem.data());

    if (!guardedItem) {
        saveError = QObject::tr("Cannot open the activity for saving");
        return false;
    }

    QPointer<RideFile> guardedRide(
        guardedItem->ride());
    const auto workflowIsCurrent = [&] {
        RideItem *const item = guardedItem.data();
        const bool relationshipsMatch =
            !contextRequired
            || (guardedContext && guardedAthlete
                && guardedCache
                && guardedContext->athlete
                    == guardedAthlete.data()
                && guardedAthlete->rideCache
                    == guardedCache.data());
        const bool itemInCache = membershipRequired
            && guardedCache && item
            && activitySaveCacheContainsUniqueIdentity(
                guardedCache.data(), item,
                expectedIdentity);
        return ActivitySaveWorkflow::isCurrent(
                guardedRide != nullptr,
                contextRequired,
                guardedContext != nullptr,
                guardedAthlete != nullptr,
                guardedCache != nullptr,
                relationshipsMatch,
                item != nullptr,
                membershipRequired,
                itemInCache,
                expectedIdentity,
                activitySaveIdentity(item))
            && item->ride(false)
                == guardedRide.data();
    };
    const auto rejectChangedWorkflow =
        [&](const QString &message) {
            if (workflowIsCurrent()) return false;
            if (saveError.isEmpty()) saveError = message;
            return true;
        };

    if (rejectChangedWorkflow(QObject::tr(
            "The activity changed before it could be saved"))) {
        return false;
    }

    const QString sourcePath =
        QFileInfo(QDir(expectedIdentity.path).filePath(
            expectedIdentity.fileName))
            .absoluteFilePath();
    const QFileInfo sourceInfo(sourcePath);
    const bool convert = sourceInfo.completeSuffix().compare(
                             QStringLiteral("json"), Qt::CaseInsensitive) != 0;

    const QDateTime rideDateTime =
        guardedRide->startTime();
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
    saveOperations.persistCompletesDurableTransaction =
        !pathChanges;
    if (!saveOperations.timestamp.isValid()) {
        saveOperations.timestamp = QDateTime::currentDateTime();
    }

    const std::function<bool(RideFile *, QString &)>
        requestedStage = saveOperations.stage;
    saveOperations.stage =
        [requestedStage, guardedRide,
         &workflowIsCurrent](
            RideFile *activity, QString &stageError) {
            if (!workflowIsCurrent()
                || activity != guardedRide.data()) {
                stageError = QObject::tr(
                    "The activity changed before save processing");
                return false;
            }
            const bool staged = !requestedStage
                || requestedStage(activity, stageError);
            if (!workflowIsCurrent()) {
                if (stageError.isEmpty()) {
                    stageError = QObject::tr(
                        "The activity changed during save processing");
                }
                return false;
            }
            return staged;
        };

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    saveOperations.finalize = [&](QString &stepError) {
        if (!workflowIsCurrent()) {
            stepError = QObject::tr(
                "The activity changed before save finalization");
            return false;
        }
        if (pathChanges && !atomicFileMatchesSnapshot(
                sourcePath, sourceSnapshot, stepError)) {
            return false;
        }
        if (!finalizeActivityFileReplacement(sourcePath, targetPath,
                                             convert, stepError)) {
            return false;
        }
        if (pathChanges) {
            guardedItem->setFileName(
                sourceInfo.absolutePath(),
                QFileInfo(targetPath).fileName());
            expectedIdentity.path =
                sourceInfo.absolutePath();
            expectedIdentity.fileName =
                QFileInfo(targetPath).fileName();
        }
        if (!workflowIsCurrent()) {
            stepError = QObject::tr(
                "The activity changed while the save was being finalized");
            return false;
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
    saveOperations.markClean =
        [guardedRide, &workflowIsCurrent]() {
            if (workflowIsCurrent() && guardedRide)
                guardedRide->emitSaved();
        };

    const bool saved = saveActivityTransaction(
        guardedContext.data(), guardedRide.data(),
        targetPath, saveOperations, saveError);

    if (saved && workflowIsCurrent()) {
        QFile notesFile(QDir(sourceInfo.absolutePath()).filePath(
            sourceInfo.baseName() + QStringLiteral(".notes")));
        if (notesFile.exists()) {
            notesFile.remove();
        }
        QPointer<Estimator> estimator(
            guardedCache
                ? guardedCache->estimator
                : nullptr);
        if (estimator) estimator->refresh();
    }

    QGuiApplication::restoreOverrideCursor();
    return saved;
}


void
MainWindow::saveAllFilesSilent(Context *context)
{
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete ? guardedAthlete->rideCache : nullptr);
    if (!guardedContext || !guardedAthlete || !guardedCache) {
        return;
    }

    // iterate over snapshot of rides to prevent crash by iterator invalidation
    QList<QPointer<RideItem>> snapshot;
    for (RideItem *rideItem :
         guardedCache->rides()) {
        snapshot.append(QPointer<RideItem>(rideItem));
    }
    for (const QPointer<RideItem> &guard : snapshot) {
        if (!guardedContext || !guardedAthlete
            || !guardedCache
            || guardedContext->athlete
                != guardedAthlete.data()
            || guardedAthlete->rideCache
                != guardedCache.data()) {
            return;
        }
        RideItem *const rideItem = guard.data();
        if (!rideItem) continue;
        if (rideItem->isDirty()) {
            this->saveRideSingleDialog(
                guardedContext.data(), rideItem);
        }
    }
}

//----------------------------------------------------------------------
// Save Single File Dialog Widget
//----------------------------------------------------------------------
SaveSingleDialogWidget::SaveSingleDialogWidget(MainWindow *mainWindow, Context *context, RideItem *rideItem) :
    QDialog(mainWindow, Qt::Dialog), context(context),
    athlete(context ? context->athlete : nullptr),
    cache(athlete ? athlete->rideCache : nullptr), rideItem(rideItem),
    identity(activitySaveIdentity(rideItem)),
    contextRequired_(context != nullptr)
{
    setWindowTitle(tr("Save and Conversion"));
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Warning text
    const QString fileName = this->rideItem
        ? this->rideItem->fileName
        : tr("an unavailable activity");
    warnText = new QLabel(tr("WARNING\n\nYou have made changes to ") + fileName + tr(" If you want to save\nthem, we need to convert to GoldenCheetah\'s\nnative format. Should we do so?\n"));
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
SaveSingleDialogWidget::activityIsCurrent() const
{
    RideItem *const item = rideItem.data();
    const bool relationshipsMatch =
        !contextRequired_
        || (context && athlete && cache
            && context->athlete == athlete.data()
            && athlete->rideCache == cache.data());
    const bool itemInCache = contextRequired_
        && cache && item
        && activitySaveCacheContainsUniqueIdentity(
            cache.data(), item, identity);

    return ActivitySaveWorkflow::isCurrent(
        true, contextRequired_, context != nullptr,
        athlete != nullptr, cache != nullptr,
        relationshipsMatch, item != nullptr,
        contextRequired_, itemInCache, identity,
        activitySaveIdentity(item));
}

bool
SaveSingleDialogWidget::saveRide(QString &error)
{
    if (contextRequired_ && !context) {
        error = tr("The activity collection is no longer available");
        return false;
    }
    return MainWindow::saveSilent(
        context.data(), rideItem.data(), &error);
}

void
SaveSingleDialogWidget::reportSaveError(const QString &error)
{
    QMessageBox::warning(this, tr("Save Activity"), error);
}

void
SaveSingleDialogWidget::saveClicked()
{
    if (!activityIsCurrent()) {
        reportSaveError(tr(
            "The selected activity is no longer available"));
        return;
    }

    QPointer<SaveSingleDialogWidget> dialog(this);
    QString error;
    const bool saved = saveRide(error);
    if (!dialog) return;
    if (!saved) {
        if (error.isEmpty()) {
            error = tr(
                "The activity could not be saved");
        }
        dialog->reportSaveError(error);
        return;
    }
    dialog->mayProceed_ = true;
    dialog->accept();
}

void
SaveSingleDialogWidget::abandonClicked()
{
    if (!activityIsCurrent()) {
        reportSaveError(tr(
            "The selected activity is no longer available"));
        return;
    }
    RideItem *const item = rideItem.data();
    QPointer<SaveSingleDialogWidget> dialog(this);
    item->setDirty(false); // lose changes
    if (!dialog) return;
    dialog->mayProceed_ = true;
    dialog->reject();
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

SaveOnExitDialogWidget::SaveOnExitDialogWidget(MainWindow *mainWindow, Context *context, QList<RideItem *>requestedDirtyList) :
    QDialog(mainWindow, Qt::Dialog), context(context),
    athlete(context ? context->athlete : nullptr),
    cache(athlete ? athlete->rideCache : nullptr),
    contextRequired_(context != nullptr)
{
    for (RideItem *rideItem : requestedDirtyList) {
        if (rideItem) {
            DirtyActivity activity;
            activity.item = rideItem;
            activity.identity =
                activitySaveIdentity(rideItem);
            dirtyList.append(activity);
        }
    }
    setWindowTitle(tr("Save Changes"));
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Warning text
    const QString athleteName =
        this->context && this->context->athlete
            ? this->context->athlete->cyclist
            : QString();
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
        RideItem *const rideItem =
            dirtyList.at(i).item.data();
        t->setText(rideItem
            ? rideItem->fileName
            : tr("Unavailable activity"));
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

QList<RideItem *>
SaveOnExitDialogWidget::currentDirtyActivities() const
{
    QList<RideItem *> dirty;
    if (contextRequired_) {
        if (!context || !athlete || !cache
            || context->athlete != athlete.data()
            || athlete->rideCache != cache.data()) {
            return dirty;
        }
        for (RideItem *item : cache->rides()) {
            if (item && item->isDirty()) dirty.append(item);
        }
        return dirty;
    }

    for (const DirtyActivity &activity : dirtyList) {
        if (activity.item && activity.item->isDirty())
            dirty.append(activity.item.data());
    }
    return dirty;
}

void
SaveOnExitDialogWidget::appendDirtyActivity(RideItem *rideItem)
{
    if (!rideItem) return;
    for (const DirtyActivity &activity :
         std::as_const(dirtyList)) {
        if (activity.item.data() == rideItem) return;
    }

    DirtyActivity activity;
    activity.item = rideItem;
    activity.identity = activitySaveIdentity(rideItem);
    const int row = dirtyList.size();
    dirtyList.append(activity);
    dirtyFiles->insertRow(row);

    QCheckBox *checkBox = new QCheckBox;
    checkBox->setCheckState(Qt::Checked);
    dirtyFiles->setCellWidget(row, 0, checkBox);

    QTableWidgetItem *fileName = new QTableWidgetItem(
        rideItem->fileName);
    fileName->setFlags(
        fileName->flags() & ~Qt::ItemIsEditable);
    dirtyFiles->setItem(row, 1, fileName);
    dirtyFiles->resizeColumnToContents(0);
    dirtyFiles->resizeColumnToContents(1);
}

bool
SaveOnExitDialogWidget::reconcileDirtyActivities(
    bool &blocksAcceptance)
{
    blocksAcceptance = false;
    for (int row = 0; row < dirtyList.size(); ++row) {
        DirtyActivity &activity = dirtyList[row];
        if (!activity.completed || !activity.item) continue;
        if (!activityIsCurrent(row)) {
            reportSaveError(tr(
                "A saved activity changed before exit could complete"));
            return false;
        }
        if (activity.item->isDirty()) {
            activity.completed = false;
            blocksAcceptance = true;
        }
    }

    const QList<RideItem *> dirtyNow =
        currentDirtyActivities();
    for (RideItem *item : dirtyNow) {
        if (!item || !item->isDirty()) continue;
        bool alreadyListed = false;
        for (const DirtyActivity &activity :
             std::as_const(dirtyList)) {
            if (activity.item.data() == item) {
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed) {
            appendDirtyActivity(item);
            blocksAcceptance = true;
        }
    }
    return true;
}

bool
SaveOnExitDialogWidget::activityIsCurrent(int row) const
{
    if (row < 0 || row >= dirtyList.count()) return false;

    const DirtyActivity &activity = dirtyList.at(row);
    RideItem *const item = activity.item.data();
    const bool relationshipsMatch =
        !contextRequired_
        || (context && athlete && cache
            && context->athlete == athlete.data()
            && athlete->rideCache == cache.data());
    const bool itemInCache = contextRequired_
        && cache && item
        && activitySaveCacheContainsUniqueIdentity(
            cache.data(), item, activity.identity);

    return ActivitySaveWorkflow::isCurrent(
        true, contextRequired_, context != nullptr,
        athlete != nullptr, cache != nullptr,
        relationshipsMatch, item != nullptr,
        contextRequired_, itemInCache,
        activity.identity, activitySaveIdentity(item));
}

bool
SaveOnExitDialogWidget::saveRide(RideItem *rideItem)
{
    if (contextRequired_ && !context) return false;
    return MainWindow::saveRideSingleDialog(
        context.data(), rideItem);
}

void
SaveOnExitDialogWidget::reportSaveError(const QString &error)
{
    QMessageBox::warning(this, tr("Save Activity"), error);
}

void
SaveOnExitDialogWidget::saveClicked()
{
    QPointer<SaveOnExitDialogWidget> dialog(this);
    QList<int> skippedRows;
    for (int i = 0; i < dialog->dirtyList.count(); ++i) {
        if (dialog->dirtyList.at(i).completed) continue;
        if (!dialog->activityIsCurrent(i)) {
            dialog->reportSaveError(tr(
                "A selected activity is no longer available"));
            return;
        }

        RideItem *const rideItem =
            dialog->dirtyList.at(i).item.data();
        QCheckBox *checkBox =
            qobject_cast<QCheckBox *>(
                dialog->dirtyFiles->cellWidget(i, 0));
        if (checkBox && checkBox->isChecked()) {
            if (!rideItem->isDirty()) {
                continue;
            }
            const bool saved = dialog->saveRide(rideItem);
            if (!dialog) return;
            if (!saved) return;
            dialog->dirtyList[i].completed = true;
            if (dialog->dirtyList.at(i).item) {
                dialog->dirtyList[i].identity =
                    activitySaveIdentity(
                        dialog->dirtyList.at(i).item.data());
            }
        } else {
            skippedRows.append(i);
        }
    }

    bool blocksAcceptance = false;
    const bool reconciled =
        dialog->reconcileDirtyActivities(
            blocksAcceptance);
    if (!dialog || !reconciled
        || blocksAcceptance) {
        return;
    }

    for (int row : std::as_const(skippedRows)) {
        if (!dialog->activityIsCurrent(row)) {
            dialog->reportSaveError(tr(
                "A selected activity is no longer available"));
            return;
        }
    }
    for (int row : std::as_const(skippedRows)) {
        dialog->dirtyList.at(row).item->skipsave = true;
    }
    dialog->accept();
}

void
SaveOnExitDialogWidget::abandonClicked()
{
    QPointer<SaveOnExitDialogWidget> dialog(this);
    bool blocksAcceptance = false;
    const bool reconciled =
        dialog->reconcileDirtyActivities(
            blocksAcceptance);
    Q_UNUSED(blocksAcceptance);
    if (!dialog || !reconciled) return;

    QList<int> abandonedRows;
    for (int i = 0; i < dialog->dirtyList.count(); ++i) {
        if (dialog->dirtyList.at(i).completed) continue;
        if (!dialog->activityIsCurrent(i)) {
            dialog->reportSaveError(tr(
                "A selected activity is no longer available"));
            return;
        }
        abandonedRows.append(i);
    }

    // we need to ensure the ride is refreshed when we restart
    // so mark the ride item as nosave to ensure rebuild
    for (int row : std::as_const(abandonedRows)) {
        dialog->dirtyList.at(row).item->skipsave = true;
    }

    dialog->accept();
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


GuardedOperationPreflightItems
guardOperationPreflightItems(
    const QList<RideItem*> &items)
{
    GuardedOperationPreflightItems guarded;
    guarded.reserve(items.size());
    for (RideItem *item : items)
        guarded.append(
            GuardedOperationPreflightItem(item));
    return guarded;
}


bool
resolveOperationPreflightItems(
    const GuardedOperationPreflightItems &guardedItems,
    QList<RideItem*> &items,
    QString &error)
{
    items.clear();
    error.clear();
    for (const GuardedOperationPreflightItem &guard :
         guardedItems) {
        RideItem *const item = guard.data();
        if (!item) {
            items.clear();
            error = QObject::tr(
                "A modified activity is no longer available");
            return false;
        }
        if (!guard.matches()) {
            items.clear();
            error = QObject::tr(
                "A modified activity changed while preparing it for saving");
            return false;
        }
        if (!items.contains(item))
            items.append(item);
    }
    if (items.isEmpty()) {
        error = QObject::tr(
            "No modified activities are available");
        return false;
    }
    return true;
}


bool
saveOperationPreflightActivities(
    const GuardedOperationPreflightItems &dirtyItems,
    const OperationPreflightRelink &relink,
    const OperationPreflightSave &save,
    QString &error,
    const OperationPreflightValidate &validate)
{
    error.clear();
    if (!relink || !save) {
        error = QObject::tr(
            "Cannot prepare modified activities for saving");
        return false;
    }
    const auto operationIsValid = [&]() {
        if (!validate) return true;
        QString validationError;
        if (validate(validationError)) return true;
        if (error.isEmpty()) {
            error = validationError.isEmpty()
                ? QObject::tr(
                    "The activity operation is no longer available")
                : validationError;
        }
        return false;
    };
    if (!operationIsValid()) return false;

    GuardedOperationPreflightItems activities =
        dirtyItems;
    QList<RideItem*> relinkItems;
    if (!resolveOperationPreflightItems(
            activities, relinkItems, error)) {
        return false;
    }
    for (RideItem *item : relinkItems) {
        const bool relinked = relink(
            item, activities, error);
        if (!operationIsValid()) return false;
        if (!relinked)
            return false;
        QList<RideItem*> validated;
        if (!resolveOperationPreflightItems(
                activities, validated, error)) {
            return false;
        }
    }

    QList<RideItem*> uniqueActivities;
    if (!resolveOperationPreflightItems(
            activities,
            uniqueActivities,
            error)) {
            return false;
    }
    if (!operationIsValid()) return false;
    const bool saved = save(uniqueActivities, error);
    if (!operationIsValid()) return false;
    return saved;
}


bool
reloadOperationPreflightActivities(
    const GuardedOperationPreflightItems &guardedItems,
    const OperationPreflightReload &reload,
    QString &error,
    const OperationPreflightValidate &validate)
{
    error.clear();
    if (!reload) {
        error = QObject::tr(
            "No activity reload operation is available");
        return false;
    }
    const auto operationIsValid = [&] {
        if (!validate) return true;
        QString validationError;
        if (validate(validationError)) return true;
        error = validationError.isEmpty()
            ? QObject::tr(
                "The activity operation is no longer available")
            : validationError;
        return false;
    };

    QList<RideItem *> resolvedItems;
    if (!operationIsValid()
        || !resolveOperationPreflightItems(
            guardedItems, resolvedItems, error)) {
        return false;
    }

    for (const GuardedOperationPreflightItem &guard :
         guardedItems) {
        if (!operationIsValid()) return false;
        RideItem *const item = guard.data();
        if (!item || !guard.matches()) {
            error = QObject::tr(
                "A discarded activity changed before it could be reloaded");
            return false;
        }

        RideFile *const reloaded = reload(item);
        if (!operationIsValid()) return false;
        if (!guard.matches()) {
            error = QObject::tr(
                "A discarded activity changed while it was being reloaded");
            return false;
        }
        if (!reloaded || item->ride(false) != reloaded) {
            error = QObject::tr(
                "The discarded activity could not be reloaded");
            return false;
        }
    }
    return true;
}


RideFile *
reloadDiscardedActivity(RideItem *item)
{
    QPointer<RideItem> guardedItem(item);
    if (!guardedItem) return nullptr;
    guardedItem->close();
    if (!guardedItem) return nullptr;
    RideFile *const reloaded = guardedItem->ride();
    return guardedItem ? reloaded : nullptr;
}


bool
proceedDialog
(Context *context, const RideCache::OperationPreCheck &check,
 ProceedDialogResult *result)
{
    if (result) result->savedActivities.clear();
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete ? guardedAthlete->rideCache : nullptr);
    QPointer<MainWindow> guardedMainWindow(
        guardedContext ? guardedContext->mainWindow : nullptr);
    const bool mainWindowRequired =
        guardedContext && guardedContext->mainWindow;
    const auto operationAvailable =
        [&](QString &validationError) {
            if (!guardedContext || !guardedAthlete
                || !guardedCache
                || guardedContext->athlete
                    != guardedAthlete.data()
                || guardedAthlete->rideCache
                    != guardedCache.data()
                || (mainWindowRequired
                    && (!guardedMainWindow
                        || guardedContext->mainWindow
                            != guardedMainWindow.data()))) {
                if (validationError.isEmpty()) {
                    validationError = QObject::tr(
                        "The activity collection is no longer available");
                }
                return false;
            }
            return true;
        };
    QString validationError;
    if (!operationAvailable(validationError)) return false;

    if (check.requiresUserDecision) {
        const GuardedOperationPreflightItems dirtyItems =
            guardOperationPreflightItems(
                check.dirtyItems);
        QPointer<QMessageBox> msgBox(new QMessageBox(
            QMessageBox::Question,
            QObject::tr("Modified activities"),
            check.warningMessage,
            QMessageBox::Save | QMessageBox::Discard
                | QMessageBox::Cancel,
            guardedMainWindow.data()));
        const int action = msgBox->exec();
        if (msgBox) delete msgBox.data();
        if (!operationAvailable(validationError)) return false;
        if (action == QMessageBox::Cancel) {
            return false;
        } else if (action == QMessageBox::Save) {
            QString error;
            QList<ProceedDialogSavedActivity> savedActivities;
            if (!saveOperationPreflightActivities(
                    dirtyItems,
                    [guardedContext](
                        RideItem *item,
                        GuardedOperationPreflightItems &activities,
                        QString &relinkError) {
                        return guardedContext
                            && relinkRideItems(
                                guardedContext.data(),
                                item, activities,
                                relinkError);
                    },
                    [guardedContext, guardedAthlete,
                     guardedCache, &savedActivities](
                        const QList<RideItem*> &items,
                        QString &saveError) {
                        if (!guardedContext
                            || !guardedAthlete
                            || !guardedCache
                            || guardedContext->athlete
                                != guardedAthlete.data()
                            || guardedAthlete->rideCache
                                != guardedCache.data()) {
                            saveError = QObject::tr(
                                "The activity collection is no longer available");
                            return false;
                        }
                        return RideCache::saveActivities(
                            guardedContext.data(), items,
                            saveError,
                            [&savedActivities](
                               Context *saveContext,
                               RideItem *saveItem,
                               QString *itemError) {
                                QPointer<RideItem> guardedItem(
                                    saveItem);
                                const bool saved =
                                    MainWindow::saveSilent(
                                        saveContext, saveItem,
                                        itemError);
                                if (saved && guardedItem) {
                                    ProceedDialogSavedActivity evidence;
                                    evidence.item = guardedItem;
                                    evidence.identity =
                                        activitySaveIdentity(
                                            guardedItem.data());
                                    savedActivities.erase(
                                        std::remove_if(
                                            savedActivities.begin(),
                                            savedActivities.end(),
                                            [guardedItem](
                                                const ProceedDialogSavedActivity &existing) {
                                                return existing.item
                                                    == guardedItem;
                                            }),
                                        savedActivities.end());
                                    savedActivities.append(evidence);
                                }
                                return saved;
                            },
                            [guardedCache](
                                RideItem *savedItem) {
                                if (!guardedCache) return;
                                QMetaObject::invokeMethod(
                                    guardedCache.data(),
                                    "itemSaved",
                                    Qt::DirectConnection,
                                    Q_ARG(
                                        RideItem *,
                                        savedItem));
                            });
                    },
                    error, operationAvailable)) {
                if (!guardedContext) return false;
                QMessageBox::warning(
                    guardedMainWindow.data(),
                    QObject::tr("Save Activity"), error);
                return false;
            }
            if (result) {
                result->savedActivities =
                    std::move(savedActivities);
            }
        } else if (action == QMessageBox::Discard) {
            QString error;
            if (!reloadOperationPreflightActivities(
                    dirtyItems,
                    reloadDiscardedActivity,
                    error,
                    operationAvailable)) {
                QMessageBox::warning(
                    guardedMainWindow.data(),
                    QObject::tr("Modified activities"),
                    error);
                return false;
            }
        }
    }
    return operationAvailable(validationError);
}


bool
relinkRideItems
(Context *context, RideItem *rideItem,
 GuardedOperationPreflightItems &activities,
 QString &error)
{
    error.clear();
    QPointer<Context> guardedContext(context);
    QPointer<RideItem> source(rideItem);
    QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete ? guardedAthlete->rideCache : nullptr);
    QPointer<MainWindow> mainWindow(
        guardedContext
            ? guardedContext->mainWindow
            : nullptr);
    const ActivitySaveWorkflow::Identity sourceIdentity =
        activitySaveIdentity(source.data());
    const auto itemIsCurrent =
        [&](const QPointer<RideItem> &guardedItem,
            const ActivitySaveWorkflow::Identity &expected) {
            RideItem *const item = guardedItem.data();
            const bool relationshipsMatch =
                guardedContext && guardedAthlete
                && guardedCache && mainWindow
                && guardedContext->athlete
                    == guardedAthlete.data()
                && guardedAthlete->rideCache
                    == guardedCache.data()
                && guardedContext->mainWindow
                    == mainWindow.data();
            const bool itemInCache =
                relationshipsMatch && item
                && activitySaveCacheContainsUniqueIdentity(
                    guardedCache.data(), item, expected);
            return ActivitySaveWorkflow::isCurrent(
                mainWindow != nullptr, true,
                guardedContext != nullptr,
                guardedAthlete != nullptr,
                guardedCache != nullptr,
                relationshipsMatch,
                item != nullptr, true, itemInCache,
                expected, activitySaveIdentity(item));
        };
    const auto relinkAvailable =
        [&](const QString &message) {
            if (itemIsCurrent(
                    source, sourceIdentity)) {
                return true;
            }
            if (error.isEmpty()) error = message;
            return false;
        };
    if (!relinkAvailable(QObject::tr(
            "An activity is no longer available for relinking"))) {
        return false;
    }

    QString newFilename;
    bool hasNewFilename = mainWindow->filenameWillChange(
        source.data(), &newFilename);
    if (!relinkAvailable(QObject::tr(
            "An activity changed while preparing linked filenames"))) {
        return false;
    }
    const bool expectsLinkedActivity =
        !source->getLinkedFileName().isEmpty();
    if (!relinkAvailable(QObject::tr(
            "An activity changed while checking its linked activity"))) {
        return false;
    }
    QPointer<RideItem> linkedItem(
        guardedCache->getLinkedActivity(source.data()));
    if (!relinkAvailable(QObject::tr(
            "An activity changed while checking its linked activity"))) {
        return false;
    }
    const ActivitySaveWorkflow::Identity linkedIdentity =
        activitySaveIdentity(linkedItem.data());
    const bool linkedItemRequired =
        linkedItem != nullptr;
    const auto pairIsCurrent = [&] {
        return relinkAvailable(QObject::tr(
                   "An activity changed while updating linked activities"))
            && (!linkedItemRequired
                || itemIsCurrent(
                    linkedItem, linkedIdentity));
    };
    if (expectsLinkedActivity
        && (!linkedItem
            || linkedItem == source
            || !itemIsCurrent(
                linkedItem, linkedIdentity)
            || linkedItem->getLinkedFileName()
                != source->fileName)) {
        error = QObject::tr(
            "The linked activity pair is missing or inconsistent");
        return false;
    }
    if (linkedItem) {
        QString linkedNewFilename;
        bool hasLinkedNewFilename =
            mainWindow->filenameWillChange(
                linkedItem.data(),
                &linkedNewFilename);
        if (!pairIsCurrent()) {
            if (error.isEmpty()) error = QObject::tr(
                "A linked activity changed while preparing filenames");
            return false;
        }
        if (hasNewFilename) {
            const bool alreadyIncluded = std::any_of(
                activities.cbegin(), activities.cend(),
                [&linkedItem](
                    const GuardedOperationPreflightItem &guard) {
                    return guard.data() == linkedItem.data();
                });
            if (!alreadyIncluded) {
                activities.append(
                    GuardedOperationPreflightItem(
                        linkedItem.data()));
            }
            linkedItem->setLinkedFileName(newFilename);
            if (!pairIsCurrent()) {
                if (error.isEmpty()) error = QObject::tr(
                    "A linked activity changed while updating its filename");
                return false;
            }
            linkedItem->setDirty(true);
            if (!pairIsCurrent()) {
                if (error.isEmpty()) error = QObject::tr(
                    "A linked activity changed while marking it for saving");
                return false;
            }
        }
        if (hasLinkedNewFilename) {
            source->setLinkedFileName(
                linkedNewFilename);
            if (!pairIsCurrent()) {
                if (error.isEmpty()) error = QObject::tr(
                    "An activity changed while updating its linked filename");
                return false;
            }
            source->setDirty(true);
            if (!pairIsCurrent()) {
                if (error.isEmpty()) error = QObject::tr(
                    "An activity changed while marking it for saving");
                return false;
            }
        }
    }
    return true;
}
