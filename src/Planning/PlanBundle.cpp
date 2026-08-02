/*
 * Copyright (c) 2026 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
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

#include "PlanBundle.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <memory>
#include <utility>
#include <vector>

#include "AtomicFileWriter.h"
#include "Athlete.h"
#include "AthleteTab.h"
#include "PlanBundleImportJournal.h"
#include "RideCache.h"
#include "TrainDB.h"
#include "ErgFile.h"

#include "../qzip/zipreader.h"
#include "../qzip/zipwriter.h"

static std::pair<int, int> writeActivities(Context *context, const PlanExportDescription &description, const QDir &activityWorkDir, const QDir &workoutWorkDir, PlanMetadata &metadata);
static bool packDir(const QDir &workDir, const QString &dirName, ZipWriter &zipWriter);
static QString hashFile(const QString &filePath);
static QString updateTags(RideFile *rideFile, const QDate &newDate, const QString &planName);
static bool scaleOverride(QMap<QString, QMap<QString, QString>> &overrides, const QString &key, int fromCP, int toCP);

static constexpr int baselinePower = 300;
static constexpr int md5HashLength = 32;
static constexpr qint64 maximumPlanWorkoutSize =
    64LL * 1024 * 1024;
static constexpr qint64 maximumPlanWorkoutPayload =
    256LL * 1024 * 1024;


namespace {

class ScopedPlanRideFiles final
{
public:
    ~ScopedPlanRideFiles()
    {
        for (const QPointer<RideFile> &ride :
             std::as_const(files_)) {
            if (ride) delete ride.data();
        }
    }

    void append(RideFile *ride)
    {
        files_.append(QPointer<RideFile>(ride));
    }

private:
    QVector<QPointer<RideFile>> files_;
};


struct WorkoutPreparationState
{
    QString workoutRoot;
    QHash<QString, QString> databaseHashes;
    QHash<QString, QString> resolvedHashes;
    QHash<QString, QByteArray> contentsByHash;
    QHash<QString, QByteArray> contentsByReference;
    QSet<QString> reservedTargetKeys;
    QList<TrainDB::PlanImportWorkout> imports;
    qint64 aggregatePayloadSize = 0;
    bool initialized = false;
};


bool directRegularFilePath(
    const QDir &directory,
    const QString &fileName,
    QString &path,
    QString &error)
{
    path.clear();
    if (fileName.isEmpty()
        || QFileInfo(fileName).fileName() != fileName
        || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\'))) {
        error = QObject::tr("A plan bundle file name is invalid");
        return false;
    }

    const QString root = directory.canonicalPath();
    const QFileInfo info(directory.filePath(fileName));
    const QString parent = QFileInfo(
        info.absolutePath()).canonicalFilePath();
    const QString absolute = QDir::cleanPath(
        info.absoluteFilePath());
    if (root.isEmpty() || parent.isEmpty()
        || info.isSymLink() || !info.exists()
        || !info.isFile()
        || atomicFilePathKey(parent)
            != atomicFilePathKey(root)
        || atomicFilePathKey(absolute)
            != atomicFilePathKey(
                QDir(root).filePath(fileName))) {
        error = QObject::tr(
            "A plan bundle file is unavailable or unsafe: %1")
                    .arg(fileName);
        return false;
    }
    path = absolute;
    return true;
}


bool readBoundedFile(
    const QString &path,
    QByteArray &contents,
    QString &error)
{
    contents.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot read workout %1: %2")
                    .arg(QFileInfo(path).fileName(),
                         file.errorString());
        return false;
    }
    const qint64 expectedSize = file.size();
    if (expectedSize < 0
        || expectedSize > maximumPlanWorkoutSize) {
        error = QObject::tr(
            "Workout %1 is too large to import")
                    .arg(QFileInfo(path).fileName());
        return false;
    }
    contents = file.read(maximumPlanWorkoutSize + 1);
    if (contents.size() != expectedSize
        || contents.size() > maximumPlanWorkoutSize
        || file.error() != QFileDevice::NoError) {
        contents.clear();
        error = QObject::tr(
            "Workout %1 changed or could not be read completely")
                    .arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}


bool validateWorkoutFile(
    const QString &path,
    const QByteArray &expectedContents,
    Context *context,
    QString &error)
{
    ErgFile workout(path, ErgFileFormat::unknown, context);
    if (!workout.isValid()) {
        error = QObject::tr("Workout %1 is invalid")
                    .arg(QFileInfo(path).fileName());
        return false;
    }
    QByteArray stableContents;
    if (!readBoundedFile(path, stableContents, error))
        return false;
    if (stableContents != expectedContents) {
        error = QObject::tr(
            "Workout %1 changed while it was being validated")
                    .arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}


bool resolveWorkoutRoot(
    Context *context,
    QString &workoutRoot,
    QString &error)
{
    workoutRoot.clear();
    if (!context || !context->athlete
        || !context->athlete->home) {
        error = QObject::tr(
            "The athlete is unavailable for workout import");
        return false;
    }
    QString configured = appsettings->value(
        nullptr, GC_WORKOUTDIR).toString();
    if (configured.isEmpty()) {
        QDir root = context->athlete->home->root();
        if (!root.cdUp()) {
            error = QObject::tr(
                "The workout library cannot be resolved");
            return false;
        }
        configured = root.absolutePath();
    }

    const QFileInfo info(configured);
    const QString canonical = QDir::cleanPath(
        info.canonicalFilePath());
    if (!info.exists() || !info.isDir()
        || canonical.isEmpty()) {
        error = QObject::tr(
            "The workout library is unavailable");
        return false;
    }
    workoutRoot = canonical;
    return true;
}


QString uniqueWorkoutTargetName(
    const PlanBundleWorkoutReference &reference,
    const QString &workoutRoot,
    QSet<QString> &reservedTargetKeys)
{
    const QString suffix =
        QFileInfo(reference.originalFileName).suffix();
    const QString extension = suffix.isEmpty()
        ? QString()
        : QLatin1Char('.') + suffix;
    QStringList candidates {
        reference.originalFileName,
        reference.hash + extension};
    for (const QString &candidate : std::as_const(candidates)) {
        const QString path = QDir(workoutRoot).filePath(candidate);
        const QString key = atomicFilePathKey(path);
        const QFileInfo info(path);
        if (!reservedTargetKeys.contains(key)
            && !info.exists() && !info.isSymLink()) {
            reservedTargetKeys.insert(key);
            return candidate;
        }
    }
    while (true) {
        const QString candidate = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower()
            + extension;
        const QString path = QDir(workoutRoot).filePath(candidate);
        const QString key = atomicFilePathKey(path);
        const QFileInfo info(path);
        if (!reservedTargetKeys.contains(key)
            && !info.exists() && !info.isSymLink()) {
            reservedTargetKeys.insert(key);
            return candidate;
        }
    }
}


bool prepareWorkoutReference(
    RideFile *rideFile,
    Context *context,
    TrainDB *database,
    const QDir &bundleWorkoutDir,
    WorkoutPreparationState &state,
    QString &error)
{
    const QString packedName = rideFile->getTag(
        "WorkoutFilename", "");
    if (packedName.isEmpty()) return true;

    PlanBundleWorkoutReference reference;
    if (!parsePlanBundleWorkoutReference(
            packedName, reference)) {
        error = QObject::tr(
            "Linked workout has an invalid bundle reference: %1")
                    .arg(packedName);
        return false;
    }
    if (!database) {
        error = QObject::tr(
            "The workout database is unavailable");
        return false;
    }
    if (!state.initialized) {
        const TrainDB::SchemaStatus status =
            database->schemaStatus();
        if (QThread::currentThread() != database->thread()
            || (status != TrainDB::SchemaStatus::current
                && status
                    != TrainDB::SchemaStatus::migrationReady)
            || !resolveWorkoutRoot(
                context, state.workoutRoot, error)) {
            if (error.isEmpty()) {
                error = QObject::tr(
                    "The workout database is unavailable for plan import");
            }
            return false;
        }
        state.databaseHashes = database->getWorkoutHashes();
        state.initialized = true;
    }

    QByteArray contents;
    const auto cachedReference =
        state.contentsByReference.constFind(packedName);
    if (cachedReference != state.contentsByReference.cend()) {
        contents = cachedReference.value();
    } else {
        QString sourcePath;
        if (!directRegularFilePath(
                bundleWorkoutDir, packedName,
                sourcePath, error)
            || !readBoundedFile(
                sourcePath, contents, error)
            || QCryptographicHash::hash(
                   contents, QCryptographicHash::Md5).toHex()
                != reference.hash.toLatin1()
            || !validateWorkoutFile(
                sourcePath, contents, context, error)) {
            if (error.isEmpty()) {
                error = QObject::tr(
                    "Workout %1 does not match its bundle hash")
                            .arg(packedName);
            }
            return false;
        }
        if (state.aggregatePayloadSize
                > maximumPlanWorkoutPayload - contents.size()) {
            error = QObject::tr(
                "The plan bundle contains too much workout data");
            return false;
        }
        state.aggregatePayloadSize += contents.size();
        state.contentsByReference.insert(packedName, contents);
    }

    const auto sameHash =
        state.contentsByHash.constFind(reference.hash);
    if (sameHash != state.contentsByHash.cend()
        && sameHash.value() != contents) {
        error = QObject::tr(
            "Two linked workouts use the same hash for different contents");
        return false;
    }
    if (sameHash == state.contentsByHash.cend())
        state.contentsByHash.insert(reference.hash, contents);

    const auto resolved =
        state.resolvedHashes.constFind(reference.hash);
    if (resolved != state.resolvedHashes.cend()) {
        rideFile->setTag("WorkoutFilename", resolved.value());
        return true;
    }

    const auto existing =
        state.databaseHashes.constFind(reference.hash);
    if (existing != state.databaseHashes.cend()) {
        const QString existingPath = QDir::cleanPath(
            QFileInfo(existing.value()).absoluteFilePath());
        const QFileInfo existingInfo(existingPath);
        QByteArray existingContents;
        if (!QDir::isAbsolutePath(existing.value())
            || existingInfo.isSymLink()
            || !existingInfo.exists() || !existingInfo.isFile()
            || !readBoundedFile(
                existingPath, existingContents, error)
            || existingContents != contents
            || !validateWorkoutFile(
                existingPath, existingContents,
                context, error)) {
            if (error.isEmpty()) {
                error = QObject::tr(
                    "An existing workout with hash %1 is unavailable or differs from the bundle")
                            .arg(reference.hash);
            }
            return false;
        }
        state.resolvedHashes.insert(
            reference.hash, existingPath);
        rideFile->setTag("WorkoutFilename", existingPath);
        return true;
    }

    const QString targetName = uniqueWorkoutTargetName(
        reference, state.workoutRoot,
        state.reservedTargetKeys);
    const QString targetPath =
        QDir(state.workoutRoot).filePath(targetName);
    state.imports.append({targetName, contents, {}});
    state.resolvedHashes.insert(reference.hash, targetPath);
    rideFile->setTag("WorkoutFilename", targetPath);
    return true;
}


PlanBundleImport::DatabaseCompletion planImportDatabaseCompletion(
    Context *context,
    TrainDB *database)
{
    const QPointer<Context> guardedContext(context);
    const QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    const QPointer<TrainDB> guardedDatabase(database);
    return [guardedContext, guardedAthlete, guardedDatabase](
        const TrainDB::PlanImportJournal &journal,
        QString &error) {
        const auto ownersValid = [&] {
            return guardedContext && guardedAthlete
                && guardedDatabase
                && guardedContext->athlete
                    == guardedAthlete.data()
                && guardedAthlete->context
                    == guardedContext.data()
                && QThread::currentThread()
                    == guardedDatabase->thread();
        };
        if (!ownersValid()) {
            error = QObject::tr(
                "The athlete or workout database was closed during plan recovery");
            return false;
        }

        std::vector<std::unique_ptr<ErgFile>> workouts;
        QStringList paths;
        workouts.reserve(journal.workouts.size());
        paths.reserve(journal.workouts.size());
        for (const TrainDB::PlanImportWorkout &record :
             journal.workouts) {
            QString path;
            QByteArray contents;
            if (!directRegularFilePath(
                    QDir(journal.workoutRoot),
                    record.targetFileName,
                    path, error)
                || !readBoundedFile(path, contents, error)
                || contents.size() != record.contents.size()
                || QCryptographicHash::hash(
                       contents, QCryptographicHash::Sha256)
                    != record.digest) {
                if (error.isEmpty()) {
                    error = QObject::tr(
                        "A published workout changed before database import");
                }
                return false;
            }
            auto workout = std::make_unique<ErgFile>(
                path, ErgFileFormat::unknown,
                guardedContext.data());
            if (!workout->isValid()) {
                error = QObject::tr(
                    "Published workout %1 is invalid")
                            .arg(record.targetFileName);
                return false;
            }
            QByteArray stableContents;
            if (!readBoundedFile(
                    path, stableContents, error)
                || stableContents != contents
                || !ownersValid()) {
                if (error.isEmpty()) {
                    error = QObject::tr(
                        "A published workout or its owner changed during database import");
                }
                return false;
            }
            paths.append(path);
            workouts.push_back(std::move(workout));
        }

        if (!guardedDatabase->startLUW()) {
            error = QObject::tr(
                "Cannot start the workout database transaction");
            return false;
        }
        const auto rollback = [&] {
            if (guardedDatabase)
                guardedDatabase->rollbackLUW();
            return false;
        };
        for (qsizetype index = 0;
             index < paths.size(); ++index) {
            if (!ownersValid()
                || !guardedDatabase->importWorkout(
                    paths.at(index), *workouts.at(index),
                    ImportMode::insertOrUpdate)) {
                error = QObject::tr(
                    "Workout %1 could not be written to the workout database")
                            .arg(QFileInfo(paths.at(index)).fileName());
                return rollback();
            }
        }
        if (!guardedDatabase->removePlanImportJournal(
                journal.id, error)) {
            return rollback();
        }
        const bool committed =
            guardedDatabase->endLUW();
        if (!committed) {
            error = QObject::tr(
                "The workout database transaction could not be committed");
            return rollback();
        }
        return true;
    };
}

} // namespace


////////////////////////////////////////////////////////////////////////////////
// PlanMetadata


const QString PlanMetadata::ManifestFilename = "manifest.json";
const QString PlanMetadata::ReadmeFilename = "README.md";


void
PlanMetadata::reset
()
{
    bundleVersion = -1;
    name = QString();
    author = QString();
    sport = QString();
    copyright = QString();
    description = QString();
    exportedAt = QDateTime();
    durationDays = 0;
    frontGapDays = 0;
    backGapDays = 0;
}


bool
PlanMetadata::isValid
() const
{
    return    bundleVersion == 1
           && durationDays > 0
           && frontGapDays >= 0
           && backGapDays >= 0
           && exportedAt.isValid();
}


QList<QString>
PlanMetadata::save
(const QDir &dir) const
{
    QList<QString> filePaths;
    QFile manifestFile(dir.filePath(ManifestFilename));
    if (manifestFile.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(toJson());
        manifestFile.write(doc.toJson(QJsonDocument::Indented));
        manifestFile.close();
        filePaths << ManifestFilename;

        if (description.trimmed().length() > 0) {
            QFile readmeFile(dir.filePath(ReadmeFilename));
            if (readmeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&readmeFile);
                out << description;
                readmeFile.close();
                filePaths << ReadmeFilename;
            }
        }
    }
    return filePaths;
}


bool
PlanMetadata::load
(const QDir &dir)
{
    QFile manifestFile(dir.filePath(ManifestFilename));
    if (! manifestFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray manifestData = manifestFile.readAll();
    manifestFile.close();
    QJsonParseError manifestParseError;
    QJsonDocument manifestDoc = QJsonDocument::fromJson(manifestData, &manifestParseError);
    if (manifestParseError.error != QJsonParseError::NoError) {
        return false;
    }
    fromJson(manifestDoc.object());
    if (! isValid()) {
        return false;
    }

    QFile readmeFile(dir.filePath(ReadmeFilename));
    if (readmeFile.open(QIODevice::ReadOnly)) {
        QByteArray readmeData = readmeFile.readAll();
        readmeFile.close();
        description = QString(readmeData);
    }

    return true;
}


QJsonObject
PlanMetadata::toJson
() const
{
    QJsonObject obj;
    obj["bundleVersion"] = bundleVersion;
    obj["name"] = name;
    obj["author"] = author;
    obj["sport"] = sport;
    obj["exportedAt"] = exportedAt.toString(Qt::ISODate);
    obj["durationDays"] = durationDays;
    obj["frontGapDays"] = frontGapDays;
    obj["backGapDays"] = backGapDays;
    obj["copyright"] = copyright;
    return obj;
}


bool
PlanMetadata::fromJson
(const QJsonObject &obj)
{
    bool ok = false;
    reset();
    if (! obj.contains("bundleVersion")) {
        return false;
    }
    int bv = obj["bundleVersion"].toInt();
    if (bv == 1) {
        if (   ! obj.contains("author")
            || ! obj.contains("name")) {
            return false;
        }
        bundleVersion = bv;
        name = obj["name"].toString().trimmed();
        author = obj["author"].toString().trimmed();
        sport = obj["sport"].toString().trimmed();
        exportedAt = QDateTime::fromString(obj["exportedAt"].toString().trimmed(), Qt::ISODate);
        durationDays = obj["durationDays"].toInt(0);
        frontGapDays = obj["frontGapDays"].toInt(0);
        backGapDays = obj["backGapDays"].toInt(0);
        copyright = obj["copyright"].toString().trimmed();
        ok = true;
    }
    return ok;
}


////////////////////////////////////////////////////////////////////////////////
// PlanResult

bool
PlanResult::ok
() const
{
    return errors.isEmpty();
}


void
PlanResult::addError
(const QString &error)
{
    errors << error;
}


void
PlanResult::addWarning
(const QString &warning)
{
    warnings << warning;
}


void
PlanResult::reset
()
{
    errors.clear();
    warnings.clear();
    committed = false;
}


////////////////////////////////////////////////////////////////////////////////
// RideFileSelection

RideFileSelection::RideFileSelection
(RideFile *rideFile, bool selected, const QDateTime &dt,
 const QString &sourceFileName)
: selected(selected), targetDateTime(dt), rideFile(rideFile),
  sourceFileName(sourceFileName)
{
}


RideFile*
RideFileSelection::getRideFile
() const
{
    return rideFile;
}


QString
RideFileSelection::getSourceFileName
() const
{
    return sourceFileName;
}


////////////////////////////////////////////////////////////////////////////////
// PlanBundleReader

PlanBundleReader::PlanBundleReader
(Context *context, const QDate &targetDate)
: context(context), targetRangeStart(targetDate)
{
}


PlanBundleReader::~PlanBundleReader
()
{
    reset();
}


PlanMetadata
PlanBundleReader::getMetadata
() const
{
    return metadata;
}


PlanResult
PlanBundleReader::loadBundle
(const QString &bundlePath)
{
    reset();
    lastValidationResult.reset();

    if (!context) {
        lastValidationResult.addError(
            QObject::tr(
                "The athlete was closed before the plan could be loaded"));
        return lastValidationResult;
    }

    QFileInfo fileInfo(bundlePath);
    if (! fileInfo.isFile()) {
        lastValidationResult.addError(QObject::tr("Path is not a file"));
        return lastValidationResult;
    }
    if (! fileInfo.isReadable()) {
        lastValidationResult.addError(QObject::tr("File is not readable"));
        return lastValidationResult;
    }

    tempDir = new QTemporaryDir();
    if (! tempDir->isValid()) {
        lastValidationResult.addError(QObject::tr("Failed to create temporary directory"));
        return lastValidationResult;
    }
    QDir baseDir(tempDir->path());

    ZipReader zipReader(bundlePath);
    if (! zipReader.extractAll(baseDir.absolutePath())) {
        lastValidationResult.addError(QObject::tr("Failed to unpack bundle"));
        reset();
        return lastValidationResult;
    }
    if (! metadata.load(baseDir)) {
        lastValidationResult.addError(QObject::tr("Failed to load metadata"));
        reset();
        return lastValidationResult;
    }
    plannedDir.setPath(baseDir.filePath("planned"));
    workoutDir.setPath(baseDir.filePath("workouts"));
    if (! plannedDir.exists() || ! workoutDir.exists()) {
        lastValidationResult.addError(QObject::tr("Invalid bundle structure"));
        reset();
        return lastValidationResult;
    }

    QFileInfoList plannedList = plannedDir.entryInfoList(QDir::Files);
    for (const QFileInfo &fileInfo : plannedList) {
        QString sourcePath;
        QString sourceError;
        if (!directRegularFilePath(
                plannedDir, fileInfo.fileName(),
                sourcePath, sourceError)) {
            lastValidationResult.addError(sourceError);
            reset();
            return lastValidationResult;
        }
        QFile sourceFile(sourcePath);
        QStringList errors;
        RideFile *rideFile = RideFileFactory::instance().openRideFile(context, sourceFile, errors);
        if (! rideFile) {
            lastValidationResult.addError(QObject::tr("Failed to load activity %1").arg(fileInfo.fileName()));
            reset();
            return lastValidationResult;
        }
        rideFiles << RideFileSelection {
            rideFile, true, rideFile->startTime(),
            fileInfo.fileName() };
    }
    if (rideFiles.count() == 0) {
        lastValidationResult.addWarning(QObject::tr("No activities in bundle"));
    }
    std::sort(rideFiles.begin(), rideFiles.end(), [](const RideFileSelection &a, const RideFileSelection &b) {
        return a.getRideFile()->startTime() < b.getRideFile()->startTime();
    });
    calculateRange();

    validate();
    if (! lastValidationResult.ok()) {
        reset();
        return lastValidationResult;
    }

    return lastValidationResult;
}


bool
PlanBundleReader::isValid
() const
{
    return ! isNull() && lastValidationResult.ok();
}


bool
PlanBundleReader::isNull
() const
{
    return tempDir == nullptr;
}


PlanResult
PlanBundleReader::importBundle
()
{
    lastImportResult.reset();
    if (isNull()) {
        lastImportResult.addError(QObject::tr("No bundle was loaded"));
        return lastImportResult;
    }
    if (! isValid()) {
        lastImportResult.addError(QObject::tr("Current bundle is invalid"));
        return lastImportResult;
    }
    if (getNumSelectedActivities() == 0) {
        lastImportResult.addError(
            QObject::tr("Select at least one activity to import"));
        return lastImportResult;
    }

    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    QPointer<RideCache> guardedCache(
        guardedAthlete ? guardedAthlete->rideCache : nullptr);
    QPointer<TrainDB> guardedDatabase(trainDB);
    const auto ownersValid = [&]() {
        return guardedContext && guardedAthlete
            && guardedCache
            && context == guardedContext.data()
            && guardedContext->athlete
                == guardedAthlete.data()
            && guardedAthlete->rideCache
                == guardedCache.data();
    };
    const auto requireOwners = [&]() {
        if (ownersValid())
            return true;
        lastImportResult.addError(
            QObject::tr(
                "The athlete was closed during plan import"));
        return false;
    };
    if (!requireOwners())
        return lastImportResult;

    findConflicts();
    if (!requireOwners())
        return lastImportResult;
    if (existingLinked.count() > 0) {
        for (const RideFileSelection &entry : rideFiles) {
            const PlanImportScheduleActivity activity {
                entry.getRideFile()->startTime(), entry.selected};
            if (planImportHasLinkedConflict(
                    activity, entry.targetDateTime,
                    existingLinked)) {
                lastImportResult.addError(QObject::tr("Bundle can't be imported: Conflicts with linked planned activity on %1").arg(entry.targetDateTime.toString()));
                return lastImportResult;
            }
        }
    }
    const QList<RideItem*> ridesToRemove =
        getRideItemsToRemove();
    if (!requireOwners())
        return lastImportResult;

    ScopedPlanRideFiles preparedRides;
    WorkoutPreparationState workoutState;
    QList<RideCache::PlannedActivityTarget> targets;
    targets.reserve(getNumSelectedActivities());
    QSet<QString> targetNames;
    for (const RideFileSelection &entry : rideFiles) {
        if (!entry.selected) continue;
        if (!requireOwners()) return lastImportResult;

        QString sourcePath;
        QString preparationError;
        if (!directRegularFilePath(
                plannedDir, entry.getSourceFileName(),
                sourcePath, preparationError)) {
            lastImportResult.addError(preparationError);
            return lastImportResult;
        }
        QFile source(sourcePath);
        QStringList parseErrors;
        RideFile *prepared = RideFileFactory::instance()
            .openRideFile(
                guardedContext.data(), source, parseErrors);
        if (!prepared) {
            lastImportResult.addError(
                parseErrors.isEmpty()
                ? QObject::tr("Failed to reload activity %1")
                      .arg(entry.getSourceFileName())
                : parseErrors.join(QStringLiteral("; ")));
            return lastImportResult;
        }
        preparedRides.append(prepared);
        if (prepared->startTime()
                != entry.getRideFile()->startTime()) {
            lastImportResult.addError(
                QObject::tr(
                    "Activity %1 changed after bundle validation")
                        .arg(entry.getSourceFileName()));
            return lastImportResult;
        }
        if (!prepareWorkoutReference(
                prepared, guardedContext.data(),
                guardedDatabase.data(), workoutDir,
                workoutState, preparationError)) {
            lastImportResult.addError(preparationError);
            return lastImportResult;
        }
        if (!requireOwners()) return lastImportResult;
        if (workoutState.initialized
            && (!guardedDatabase
                || QThread::currentThread()
                    != guardedDatabase->thread())) {
            lastImportResult.addError(
                QObject::tr(
                    "The workout database was closed during plan import"));
            return lastImportResult;
        }

        QString targetFileName;
        if (!prepareActivity(
                prepared, entry.targetDateTime,
                targetFileName, preparationError)) {
            lastImportResult.addError(
                preparationError.isEmpty()
                ? QObject::tr("Failed to prepare an activity")
                : preparationError);
            return lastImportResult;
        }
        if (targetNames.contains(targetFileName)) {
            lastImportResult.addError(
                QObject::tr(
                    "Two imported activities have the same target time: %1")
                        .arg(targetFileName));
            return lastImportResult;
        }
        targetNames.insert(targetFileName);

        const QPointer<RideFile> guardedRide(prepared);
        targets.append({
            targetFileName,
            [guardedContext, guardedAthlete, guardedCache,
             guardedRide, targetFileName]
            (const QString &stagingPath, QString &error) {
                const auto stable = [&] {
                    return guardedContext && guardedAthlete
                        && guardedCache && guardedRide
                        && guardedContext->athlete
                            == guardedAthlete.data()
                        && guardedAthlete->rideCache
                            == guardedCache.data();
                };
                if (!stable()) {
                    error = QObject::tr(
                        "The athlete or prepared activity was closed before staging");
                    return false;
                }
                QFile staged(stagingPath);
                if (!RideFileFactory::instance().writeRideFile(
                        guardedContext.data(),
                        guardedRide.data(), staged,
                        QStringLiteral("json"))) {
                    error = QObject::tr(
                        "Failed to stage activity %1")
                            .arg(targetFileName);
                    return false;
                }
                if (!stable()) {
                    error = QObject::tr(
                        "The athlete or prepared activity changed during staging");
                    return false;
                }
                return true;
            }});
    }

    if (!requireOwners()) return lastImportResult;
    RideCache::PlannedReplacementCoordinator coordinator;
    std::shared_ptr<PlanBundleImport::Journal> importJournal;
    if (!workoutState.imports.isEmpty()) {
        if (!guardedDatabase) {
            lastImportResult.addError(
                QObject::tr("The workout database is unavailable"));
            return lastImportResult;
        }
        QString journalError;
        importJournal = PlanBundleImport::Journal::create(
            guardedDatabase.data(),
            guardedAthlete->home->root().absolutePath(),
            workoutState.workoutRoot,
            workoutState.imports, journalError);
        if (!importJournal) {
            lastImportResult.addError(
                journalError.isEmpty()
                ? QObject::tr(
                    "Cannot prepare the durable plan import")
                : journalError);
            return lastImportResult;
        }
        const PlanBundleImport::DatabaseCompletion
            completeDatabase = planImportDatabaseCompletion(
                guardedContext.data(), guardedDatabase.data());
        coordinator.commit = [importJournal](
            const QString &journalPath,
            bool &committed,
            QString &error) {
            return importJournal->commitDecision(
                journalPath, committed, error);
        };
        coordinator.complete = [importJournal, completeDatabase](
            QString &error) {
            return importJournal->completePublishedPlan(
                completeDatabase, error);
        };
    }

    const RideCache::PlannedReplacementResult replacement =
        guardedCache->replacePlannedActivityFiles(
            ridesToRemove, {}, targets, true, coordinator);
    lastImportResult.committed = replacement.committed;
    for (const QString &warning : replacement.warnings)
        lastImportResult.addWarning(warning);

    const bool countsMatch =
        replacement.removedCount == ridesToRemove.size()
        && replacement.addedCount == targets.size();
    if (!replacement.cleanlyCompleted() || !countsMatch) {
        lastImportResult.addError(
            !replacement.error.isEmpty()
            ? replacement.error
            : (!countsMatch
                ? QObject::tr(
                    "The committed plan is not fully represented in the activity cache")
                : QObject::tr("Failed to import the plan")));
        if (replacement.committed) {
            lastImportResult.addWarning(
                QObject::tr(
                    "The import was committed. Do not import the bundle again; restart GoldenCheetah to complete recovery."));
        }
    }

    return lastImportResult;
}


QDate
PlanBundleReader::getTargetRangeStart
() const
{
    return targetRangeStart;
}


QDate
PlanBundleReader::getTargetRangeEnd
() const
{
    return targetRangeEnd;
}


int
PlanBundleReader::getDuration
() const
{
    return duration;
}


int
PlanBundleReader::getNumActivities
() const
{
    return rideFiles.count();
}


int
PlanBundleReader::getNumSelectedActivities
() const
{
    return std::count_if(rideFiles.begin(), rideFiles.end(), [](const RideFileSelection &entry) {
        return entry.selected;
    });
}


const QStringList&
PlanBundleReader::getActivitiesToRemove
() const
{
    return toDelete;
}


QList<RideItem*>
PlanBundleReader::getRideItemsToRemove
() const
{
    QList<RideItem*> rideItems;
    if (!context || !context->athlete
        || !context->athlete->rideCache) {
        return rideItems;
    }
    for (RideItem *rideItem : context->athlete->rideCache->rides()) {
        if (   rideItem == nullptr
            || ! rideItem->planned
            || rideItem->dateTime.date() < targetRangeStart
            || rideItem->dateTime.date() > targetRangeEnd) {
            continue;
        }
        if (! rideItem->hasLinkedActivity()) {
            rideItems << rideItem;
        }
    }
    return rideItems;
}


bool
PlanBundleReader::isIncludeGapDays
() const
{
    return includeGapDays;
}


void
PlanBundleReader::setIncludeGapDays
(bool includeGapDays)
{
    this->includeGapDays = includeGapDays;
    calculateRange();
}


bool
PlanBundleReader::setActivitySelected
(int index, bool selected)
{
    if (index < 0 || index >= rideFiles.size())
        return false;
    if (rideFiles[index].selected == selected)
        return true;
    rideFiles[index].selected = selected;
    calculateRange();
    return true;
}


const QSet<QDateTime>&
PlanBundleReader::getExistingLinked
() const
{
    return existingLinked;
}


PlanResult
PlanBundleReader::getLastValidationResult
() const
{
    return lastValidationResult;
}


PlanResult
PlanBundleReader::getLastImportResult
() const
{
    return lastImportResult;
}


void
PlanBundleReader::reset
()
{
    if (tempDir != nullptr) {
        delete tempDir;
        tempDir = nullptr;
    }
    metadata.reset();
    for (RideFileSelection &entry : rideFiles) {
        delete entry.getRideFile();
    }
    rideFiles.clear();
    targetRangeEnd = QDate();
    duration = 0;
    daysToAdd = 0;
    plannedDir = QDir();
    workoutDir = QDir();
    toDelete.clear();
    existingLinked.clear();
}


void
PlanBundleReader::calculateRange
()
{
    QList<PlanImportScheduleActivity> activities;
    activities.reserve(rideFiles.size());
    for (const RideFileSelection &selection :
         std::as_const(rideFiles)) {
        activities.append({
            selection.getRideFile()->startTime(),
            selection.selected});
    }
    const PlanImportSchedule schedule =
        calculatePlanImportSchedule(
            activities, targetRangeStart,
            includeGapDays, metadata.frontGapDays,
            metadata.backGapDays);
    duration = schedule.durationDays;
    daysToAdd = schedule.daysToAdd;
    targetRangeEnd = schedule.targetRangeEnd;
    for (int index = 0;
         index < rideFiles.size()
         && index < schedule.targetDateTimes.size();
         ++index) {
        rideFiles[index].targetDateTime =
            schedule.targetDateTimes[index];
    }

    findConflicts();
}



void
PlanBundleReader::findConflicts
()
{
    toDelete.clear();
    existingLinked.clear();
    if (getNumSelectedActivities() == 0
        || !context || !context->athlete
        || !context->athlete->rideCache) {
        return;
    }
    for (RideItem *rideItem : context->athlete->rideCache->rides()) {
        if (   rideItem == nullptr
            || ! rideItem->planned
            || rideItem->dateTime.date() < targetRangeStart
            || rideItem->dateTime.date() > targetRangeEnd) {
            continue;
        }
        if (! rideItem->hasLinkedActivity()) {
            toDelete << rideItem->fileName;
        } else {
            existingLinked << rideItem->dateTime;
        }
    }
}


bool
PlanBundleReader::prepareActivity
(RideFile *rideFile, const QDateTime &targetDateTime,
 QString &targetFileName, QString &error) const
{
    targetFileName.clear();
    error.clear();
    if (!rideFile || !targetDateTime.isValid()
        || !context || !context->athlete) {
        error = QObject::tr(
            "The activity or athlete is unavailable for plan import");
        return false;
    }

    targetFileName = updateTags(
        rideFile, targetDateTime.date(), metadata.name);

    Zones const * const zones = context->athlete->zones(rideFile->getTag("Sport", ""));
    if (zones != nullptr) {
        int zonerange = zones->whichRange(rideFile->startTime().date());
        if (zonerange >= 0) {
            int cp = zones->getCP(zonerange);
            scaleOverride(rideFile->metricOverrides, "average_power", baselinePower, cp);
            scaleOverride(rideFile->metricOverrides, "coggan_np", baselinePower, cp);
            scaleOverride(rideFile->metricOverrides, "skiba_xpower", baselinePower, cp);
        }
    }
    QDateTime parsedTarget;
    if (!RideFile::parseRideFileName(
            targetFileName, &parsedTarget)
        || parsedTarget.date() != targetDateTime.date()
        || parsedTarget.time().hour()
            != targetDateTime.time().hour()
        || parsedTarget.time().minute()
            != targetDateTime.time().minute()
        || parsedTarget.time().second()
            != targetDateTime.time().second()) {
        error = QObject::tr(
            "The prepared activity has an invalid target identity");
        targetFileName.clear();
        return false;
    }
    return true;
}


void
PlanBundleReader::validate
()
{
    // Not checked (done while loading):
    // 1. Is the metadata valid?
    // 2. Is at least one activity available?
    // 3. Are all activities valid?
    // 4. Are all folders available?
    // 5. Are non-activity files in the planned-folder?

    if (tempDir == nullptr) {
        lastValidationResult.addError(QObject::tr("No temporary directory set"));
        return;
    }
    QDir baseDir(tempDir->path());

    // Checked:
    // 1. Are additional folders / files present?
    // 4. Are all workouts readable?
    QStringList baseDirNames;
    for (const QFileInfo &fi : baseDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        baseDirNames.append(fi.fileName());
    }
    const QStringList mandatoryEntries = { "planned", "workouts", PlanMetadata::ManifestFilename };
    const QStringList permittedEntries = mandatoryEntries + QStringList { PlanMetadata::ReadmeFilename };

    for (const QString &mandatory : mandatoryEntries) {
        if (! baseDirNames.contains(mandatory)) {
            lastValidationResult.addError(QObject::tr("Bundle is missing required entry: %1").arg(mandatory));
            return;
        }
    }
    for (const QString &entry : baseDirNames) {
        if (! permittedEntries.contains(entry)) {
            lastValidationResult.addError(QObject::tr("Bundle contains unexpected entry: %1").arg(entry));
            return;
        }
    }

    QFileInfoList plannedDirList = plannedDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (plannedDirList.count() != rideFiles.count()) {
        lastValidationResult.addError(QObject::tr("Mismatch between the manifest and available activities"));
        return;
    }

    QFileInfoList workoutDirList = workoutDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    qint64 aggregateWorkoutSize = 0;
    QSet<QString> availableWorkouts;
    for (const QFileInfo &fileInfo : workoutDirList) {
        QString workoutPath;
        QString workoutError;
        QByteArray contents;
        PlanBundleWorkoutReference reference;
        if (!directRegularFilePath(
                workoutDir, fileInfo.fileName(),
                workoutPath, workoutError)
            || !parsePlanBundleWorkoutReference(
                fileInfo.fileName(), reference)
            || !readBoundedFile(
                workoutPath, contents, workoutError)
            || aggregateWorkoutSize
                > maximumPlanWorkoutPayload - contents.size()
            || QCryptographicHash::hash(
                   contents, QCryptographicHash::Md5).toHex()
                != reference.hash.toLatin1()
            || !validateWorkoutFile(
                workoutPath, contents, context,
                workoutError)) {
            if (workoutError.isEmpty()) {
                workoutError = QObject::tr(
                    "Bad hash or filename for workout file (%1) in bundle")
                                   .arg(fileInfo.fileName());
            }
            lastValidationResult.addError(workoutError);
            return;
        }
        aggregateWorkoutSize += contents.size();
        availableWorkouts.insert(fileInfo.fileName());
    }

    // 3. Are all workouts linked from an activity?
    // 6. Are workouts linked from an activity missing?
    QSet<QString> workouts;
    for (const RideFileSelection &entry : rideFiles) {
        QString workoutFilename = entry.getRideFile()->getTag("WorkoutFilename", "");
        if (workoutFilename.isEmpty()) {
            continue;
        }
        PlanBundleWorkoutReference reference;
        QString workoutPath;
        QString workoutError;
        if (!parsePlanBundleWorkoutReference(
                workoutFilename, reference)
            || !availableWorkouts.contains(workoutFilename)
            || !directRegularFilePath(
                workoutDir, workoutFilename,
                workoutPath, workoutError)) {
            lastValidationResult.addError(QObject::tr("Missing workout %1 linked from activity %2").arg(workoutFilename).arg(PlanBundle::getRideName(entry.getRideFile())));
            return;
        }
        workouts.insert(workoutFilename);
    }
    if (workouts != availableWorkouts) {
        lastValidationResult.addError(QObject::tr("Bundle contains unused workouts"));
        return;
    }

    int expectedDuration = 0;
    if (rideFiles.count() > 0) {
        QDate firstActivityDate = rideFiles.first().getRideFile()->startTime().date();
        QDate lastActivityDate = rideFiles.last().getRideFile()->startTime().date();
        int activitySpan = firstActivityDate.daysTo(lastActivityDate) + 1;
        expectedDuration = activitySpan + metadata.frontGapDays + metadata.backGapDays;
    }
    if (expectedDuration != metadata.durationDays) {
        lastValidationResult.addError(QObject::tr("Duration mismatch between manifest and activities"));
        return;
    }

    return;
}


////////////////////////////////////////////////////////////////////////////////
// Namespace PlanBundle


bool
PlanBundle::reconcilePendingImport
(Context *context, QString &error)
{
    error.clear();
    const QPointer<Context> guardedContext(context);
    const QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    const QPointer<TrainDB> guardedDatabase(trainDB);
    if (!guardedContext || !guardedAthlete
        || !guardedDatabase
        || guardedContext->athlete
            != guardedAthlete.data()
        || guardedAthlete->context
            != guardedContext.data()
        || !guardedAthlete->home) {
        error = QObject::tr(
            "The athlete or workout database is unavailable for plan recovery");
        return false;
    }

    const QString athleteRoot = QDir::cleanPath(
        guardedAthlete->home->root().canonicalPath());
    if (athleteRoot.isEmpty()) {
        error = QObject::tr(
            "The athlete directory is unavailable for plan recovery");
        return false;
    }
    TrainDB::PlanImportJournal pending;
    bool found = false;
    if (!guardedDatabase->loadPlanImportJournal(
            athleteRoot, pending, found, error)) {
        return false;
    }
    if (!found) return true;

    QString workoutRoot;
    if (!resolveWorkoutRoot(
            guardedContext.data(), workoutRoot, error)) {
        return false;
    }
    return PlanBundleImport::Journal::reconcileAll(
        guardedDatabase.data(), athleteRoot, workoutRoot,
        planImportDatabaseCompletion(
            guardedContext.data(), guardedDatabase.data()),
        error);
}


bool
PlanBundle::exportBundle
(Context *context, const PlanExportDescription &description)
{
    QTemporaryDir tempDir;
    tempDir.setAutoRemove(true);

    QDir baseDir(tempDir.path());
    baseDir.mkdir("planned");
    baseDir.mkdir("workouts");
    QDir plannedDir(baseDir.filePath("planned"));
    QDir workoutDir(baseDir.filePath("workouts"));

    PlanMetadata metadata;
    metadata.bundleVersion = 1;
    metadata.name = description.name;
    metadata.author = description.author;
    metadata.sport = description.sport;
    metadata.description = description.description;
    metadata.exportedAt = QDateTime::currentDateTime();
    metadata.copyright = description.copyright;

    std::pair<int, int> copied = writeActivities(context, description, plannedDir, workoutDir, metadata);
    if (planExportCopiedAllActivities(
            description.activityFiles.size(), copied.first)) {
        ZipWriter zipWriter(description.planFile);
        if (zipWriter.status() != ZipWriter::NoError) {
            return false;
        }
        zipWriter.setCompressionPolicy(ZipWriter::AutoCompress);

        if (! packDir(plannedDir, "planned", zipWriter)) {
            zipWriter.close();
            return false;
        }
        if (copied.second > 0) {
            if (! packDir(workoutDir, "workouts", zipWriter)) {
                zipWriter.close();
                return false;
            }
        }

        QList<QString> savedFiles = metadata.save(baseDir);
        for (const QString &savedFile : savedFiles) {
            QFile file(baseDir.filePath(savedFile));
            zipWriter.setCreationPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ReadGroup |
                                             QFileDevice::ReadOther);
            zipWriter.addFile(savedFile, &file);
        }

        zipWriter.close();
        if (zipWriter.status() != ZipWriter::NoError) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}


QString
PlanBundle::getRideName
(RideItem const * const rideItem)
{
    QString rideName = rideItem->getText("Route", "").trimmed();
    if (rideName.isEmpty()) {
        rideName = rideItem->getText("Workout Code", "").trimmed();
        if (rideName.isEmpty()) {
            rideName = QObject::tr("<unnamed>");
        }
    }
    return rideName;
}


QString
PlanBundle::getRideName
(RideFile const * const rideFile)
{
    QString rideName = rideFile->getTag("Route", "").trimmed();
    if (rideName.isEmpty()) {
        rideName = rideFile->getTag("Workout Code", "").trimmed();
        if (rideName.isEmpty()) {
            rideName = QObject::tr("<unnamed>");
        }
    }
    return rideName;
}


QString
PlanBundle::getRideSport
(RideItem const * const rideItem)
{
    QString sport = rideItem->sport;
    if (! rideItem->getText("SubSport", "").isEmpty()) {
        sport += " / " + rideItem->getText("SubSport", "");
    }
    return sport;
}


QString
PlanBundle::getRideSport
(RideFile const * const rideFile)
{
    QString sport = rideFile->sport();
    if (! rideFile->getTag("SubSport", "").isEmpty()) {
        sport += " / " + rideFile->getTag("SubSport", "");
    }
    return sport;
}


QDate
PlanBundle::getRideDate
(RideItem const * const rideItem, bool preferOriginal)
{
    QDate date = rideItem->dateTime.date();
    if (preferOriginal) {
        QString originalDateString = rideItem->getText("Original Date", "");
        if (! originalDateString.isEmpty()) {
            QDate originalDate = QDate::fromString(originalDateString, "yyyy/MM/dd");
            if (originalDate.isValid()) {
                date = originalDate;
            }
        }
    }
    return date;
}


QDate
PlanBundle::getRideDate
(RideFile const * const rideFile, bool preferOriginal)
{
    QDate date = rideFile->startTime().date();
    if (preferOriginal) {
        QString originalDateString = rideFile->getTag("Original Date", "");
        if (! originalDateString.isEmpty()) {
            QDate originalDate = QDate::fromString(originalDateString, "yyyy/MM/dd");
            if (originalDate.isValid()) {
                date = originalDate;
            }
        }
    }
    return date;
}


std::pair<int, int>
writeActivities
(Context *context, const PlanExportDescription &description, const QDir &activityWorkDir, const QDir &workoutWorkDir, PlanMetadata &metadata)
{
    std::pair<int, int> ret = { 0, 0 };

    QDate firstDate;
    QDate lastDate;

    for (const QString &activityFile : description.activityFiles) {
        QString sourceFilename = context->athlete->home->planned().canonicalPath() + "/" + activityFile;
        QFile sourceFile(sourceFilename);
        QStringList errors;
        RideFile *rideFile = RideFileFactory::instance().openRideFile(context, sourceFile, errors);
        if (! rideFile) {
            continue;
        }
        QDate rideDate = PlanBundle::getRideDate(rideFile, description.preferOriginal);
        if (rideDate >= description.rangeStart && rideDate <= description.rangeEnd) {
            if (! firstDate.isValid() || ! lastDate.isValid()) {
                firstDate = rideDate;
                lastDate = rideDate;
            } else {
                if (firstDate > rideDate) {
                    firstDate = rideDate;
                }
                if (lastDate < rideDate) {
                    lastDate = rideDate;
                }
            }
        } else {
            delete rideFile;
            continue;
        }
        QString targetFileName = updateTags(rideFile, rideDate, description.name);

        QString workoutFilename = rideFile->getTag("WorkoutFilename", "");
        if (! workoutFilename.isEmpty()) {
            QFile workoutFile(workoutFilename);
            QFileInfo workoutFileInfo(workoutFile);
            QString hash = hashFile(workoutFilename);
            if (hash.length() == md5HashLength) {
                QString newName = QString("%1-%2").arg(hash).arg(workoutFileInfo.fileName());
                if (workoutFile.copy(workoutWorkDir.filePath(newName))) {
                    rideFile->setTag("WorkoutFilename", newName);
                    ++ret.second;
                } else {
                    rideFile->removeTag("WorkoutFilename");
                }
            } else {
                rideFile->removeTag("WorkoutFilename");
            }
        }
        Zones const * const zones = context->athlete->zones(rideFile->getTag("Sport", ""));
        if (zones != nullptr) {
            int zonerange = zones->whichRange(rideFile->startTime().date());
            int cp = zones->getCP(zonerange);
            scaleOverride(rideFile->metricOverrides, "average_power", cp, baselinePower);
            scaleOverride(rideFile->metricOverrides, "coggan_np", cp, baselinePower);
            scaleOverride(rideFile->metricOverrides, "skiba_xpower", cp, baselinePower);
        }

        QString targetPath = activityWorkDir.filePath(targetFileName);
        if (! QFile::exists(targetPath)) {
            QFile targetFile(targetPath);
            if (RideFileFactory::instance().writeRideFile(context, rideFile, targetFile, "json")) {
                ++ret.first;
            } else {
                qWarning() << QString("Failed to write rideFile %1").arg(targetPath);
            }
        } else {
            qWarning() << QString("RideFile %1 already exists").arg(targetPath);
        }
        delete rideFile;
    }
    if (firstDate.isValid() && lastDate.isValid()) {
        metadata.frontGapDays = description.rangeStart.daysTo(firstDate);
        metadata.backGapDays = lastDate.daysTo(description.rangeEnd);
    } else {
        metadata.frontGapDays = 0;
        metadata.backGapDays = 0;
    }
    metadata.durationDays = description.rangeStart.daysTo(description.rangeEnd) + 1;
    return ret;
}


bool
packDir
(const QDir &workDir, const QString &dirName, ZipWriter &zipWriter)
{
    QStringList fileNames = workDir.entryList(QDir::Files);
    bool ret = fileNames.count() > 0;
    if (ret) {
        zipWriter.setCreationPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                         QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                         QFileDevice::ReadOther | QFileDevice::ExeOther);
        zipWriter.addDirectory(dirName + "/");
        for (const QString &fileName : fileNames) {
            QString fullPath = workDir.filePath(fileName);
            QFile file(fullPath);
            zipWriter.setCreationPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ReadGroup |
                                             QFileDevice::ReadOther);
            zipWriter.addFile(dirName + "/" + fileName, &file);
        }
        if (zipWriter.status() != ZipWriter::NoError) {
            ret = false;
        }
    }
    return ret;
}


QString
hashFile
(const QString &filePath)
{
    QFile file(filePath);
    if (! file.open(QIODevice::ReadOnly)) {
        return "";
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    while (! file.atEnd()) {
        hash.addData(file.read(8192));
    }
    return hash.result().toHex();
}


QString
updateTags
(RideFile *rideFile, const QDate &newDate, const QString &planName)
{
    QDateTime newDateTime(rideFile->startTime());
    newDateTime.setDate(newDate);
    rideFile->setStartTime(newDateTime);
    rideFile->removeTag("Athlete");
    rideFile->removeTag("Month");
    rideFile->removeTag("Weekday");
    rideFile->removeTag("Year");
    rideFile->removeTag("Original Date");
    rideFile->removeTag("Linked Filename");
    rideFile->setTag("Plan", planName);
    rideFile->setTag("Change History", "");
    QString targetFileName = rideFile->startTime().toString("yyyy_MM_dd_HH_mm_ss") + ".json";
    rideFile->setTag("Filename", targetFileName);
    return targetFileName;
}


bool
scaleOverride
(QMap<QString, QMap<QString, QString>> &overrides, const QString &key, int fromCP, int toCP)
{
    if (fromCP <= 0 || toCP <= 0) {
        return false;
    }
    if (! overrides.contains(key)) {
        return false;
    }
    QMap<QString, QString> valueMap = overrides.value(key);
    if (! valueMap.contains("value")) {
        return false;
    }
    bool ok;
    int value = valueMap.value("value").toInt(&ok);
    if (ok) {
        value = static_cast<int>(std::round(static_cast<double>(value) * toCP / fromCP));
        valueMap["value"] = QString::number(value);
        overrides[key] = valueMap;
    }
    return ok;
}
