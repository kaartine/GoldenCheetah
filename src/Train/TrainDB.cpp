/*
 * Copyright (c) 2012 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2023 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
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

#include "TrainDB.h"
#include "Library.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QSet>
#include <QSqlQueryModel>


// DB Schema Version - YOU MUST UPDATE THIS IF THE TRAIN DB SCHEMA CHANGES

// Revision History
// Rev Date         Who                What Changed
// 01  21 Dec 2012  Mark Liversedge    Initial Build
// 02  21 Feb 2023  Joachim Kohlhammer Schema-Update

#define TABLE_VERSION "version"
#define FIELDS_VERSION \
    "table_name TEXT NOT NULL UNIQUE," \
    "schema_version INTEGER NOT NULL," \
    "creation_date INTEGER NOT NULL," \
    "PRIMARY KEY(table_name)"

#define TABLE_TAGSTORE "tagstore"
#define FIELDS_TAGSTORE \
    "id INTEGER NOT NULL UNIQUE," \
    "label TEXT NOT NULL," \
    "PRIMARY KEY(id)"

#define TABLE_WORKOUT_TAG "workout_tag"
#define FIELDS_WORKOUT_TAG \
    "filepath TEXT NOT NULL," \
    "id INTEGER NOT NULL," \
    "PRIMARY KEY(filepath, id)"

#define TABLE_WORKOUT "workout"
// if: Intensity Factor
// vi: Variability Index
// xp: XPower
// ri: RelativeIntensity
// bs: BikeScore
// svi: SkibaVariabilityIndex
#define FIELDS_WORKOUT \
    "filepath TEXT NOT NULL UNIQUE," \
    "type TEXT NOT NULL," \
    "creation_date INTEGER NOT NULL DEFAULT (STRFTIME('%s', 'now'))," \
    "source TEXT," \
    "source_id TEXT," \
    "displayname TEXT NOT NULL," \
    "description TEXT," \
    "erg_subtype TEXT," \
    "erg_duration INTEGER," \
    "erg_bikestress REAL," \
    "erg_if REAL," \
    "erg_iso_power REAL," \
    "erg_vi REAL," \
    "erg_xp REAL," \
    "erg_ri REAL," \
    "erg_bs REAL," \
    "erg_svi REAL," \
    "erg_min_power INTEGER," \
    "erg_max_power INTEGER," \
    "erg_avg_power INTEGER," \
    "erg_dominant_zone INTEGER," \
    "erg_num_zones INTEGER," \
    "erg_duration_z1 REAL," \
    "erg_duration_z2 REAL," \
    "erg_duration_z3 REAL," \
    "erg_duration_z4 REAL," \
    "erg_duration_z5 REAL," \
    "erg_duration_z6 REAL," \
    "erg_duration_z7 REAL," \
    "erg_duration_z8 REAL," \
    "erg_duration_z9 REAL," \
    "erg_duration_z10 REAL," \
    "slp_distance REAL," \
    "slp_elevation INTEGER," \
    "slp_avg_grade REAL," \
    "rating INTEGER," \
    "last_run INTEGER," \
    "PRIMARY KEY(filepath)"

#define TABLE_VIDEO "video"
#define FIELDS_VIDEO \
    "filepath TEXT NOT NULL UNIQUE," \
    "creation_date INTEGER NOT NULL DEFAULT (STRFTIME('%s', 'now'))," \
    "displayname TEXT NOT NULL," \
    "PRIMARY KEY(filepath)"

#define TABLE_VIDEOSYNC "videosync"
#define FIELDS_VIDEOSYNC \
    "filepath TEXT NOT NULL UNIQUE," \
    "creation_date INTEGER NOT NULL DEFAULT (STRFTIME('%s', 'now'))," \
    "source TEXT," \
    "displayname TEXT NOT NULL," \
    "PRIMARY KEY(filepath)"


static int TrainDBSchemaVersion = 2;
TrainDB *trainDB;


namespace {

void protectDatabaseFromWrites(const QSqlDatabase &database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA query_only = ON"))) {
        qWarning() << "TrainDB: could not protect an unusable database from writes"
                   << query.lastError();
    }
}

} // namespace


extern int
workoutModelZoneIndex
(int zone, ZoneContentType zt)
{
    if (zt == ZoneContentType::percent) {
        return TdbWorkoutModelIdx::z1Percent + (zone - 1) * 2;
    } else {
        return TdbWorkoutModelIdx::z1Seconds + (zone - 1) * 2;
    }
}


TrainDB::TrainDB
(QDir home): home(home), sessionid("train")
{
    initDatabase();
}


TrainDB::~TrainDB
()
{
    if (db) {
        db->close();
        delete db;
        QSqlDatabase::removeDatabase(sessionid);
    }
}


QSqlDatabase
TrainDB::connection
() const
{
    return db->database(sessionid);
}


void
TrainDB::closeConnection
() const
{
    connection().close();
}


void
TrainDB::initDatabase
()
{
    if (connection().isOpen()) return;

    // get a connection
    db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", sessionid));
    db->setDatabaseName(home.canonicalPath() + "/trainDB");

    if (connection().isOpen()) {
        createDatabase();
    } else {
        currentSchemaStatus = SchemaStatus::readError;
        QMessageBox::critical(0,
                              qApp->translate("TrainDB","Cannot open database"),
                              qApp->translate("TrainDB","Unable to establish a database connection.\n"
                                              "This feature requires SQLite support. Please read "
                                              "the Qt SQL driver documentation for information how "
                                              "to build it.\n\n"
                                              "Click Cancel to exit."),
                              QMessageBox::Cancel);
    }
}


// rebuild effectively drops and recreates all tables
// but not the version table, since its about deleting
// user data (e.g. when rescanning their hard disk)
bool
TrainDB::rebuildDB
() const
{
    if (currentSchemaStatus != SchemaStatus::current
        && currentSchemaStatus != SchemaStatus::migrationReady) {
        return false;
    }

    QSqlDatabase database = connection();
    if (!database.transaction()) {
        currentSchemaStatus = SchemaStatus::writeError;
        protectDatabaseFromWrites(database);
        return false;
    }

    bool ok = dropAllDataTables();
    if (ok) {
        ok = createAllDataTables();
    }

    const DatabaseState expected = currentSchemaStatus == SchemaStatus::migrationReady
                                 ? DatabaseState::migrationReady
                                 : DatabaseState::current;
    if (ok) {
        ok = databaseState() == expected;
    }
    if (ok && database.commit()) {
        return true;
    }

    database.rollback();
    currentSchemaStatus = SchemaStatus::writeError;
    protectDatabaseFromWrites(database);
    return false;
}


// wipe away all data but personal data: tags, rating, etc
// To keep personal data, we are not deleting workouts table and tags related tables
bool
TrainDB::rebuildDBButUserDataTables
() const
{
    if (currentSchemaStatus != SchemaStatus::current
        && currentSchemaStatus != SchemaStatus::migrationReady) {
        return false;
    }

    QSqlDatabase database = connection();
    if (!database.transaction()) {
        currentSchemaStatus = SchemaStatus::writeError;
        protectDatabaseFromWrites(database);
        return false;
    }

    bool ok = dropTablesButUserDataTables();
    if (ok) {
        ok = createAllDataTables(); // Tables that exist are not recreated
    }

    const DatabaseState expected = currentSchemaStatus == SchemaStatus::migrationReady
                                 ? DatabaseState::migrationReady
                                 : DatabaseState::current;
    if (ok) {
        ok = databaseState() == expected;
    }
    if (ok && database.commit()) {
        return true;
    }

    database.rollback();
    currentSchemaStatus = SchemaStatus::writeError;
    protectDatabaseFromWrites(database);
    return false;
}



///////////////////////// Implementation of TagStore


void
TrainDB::deferTagSignals
(bool deferred)
{
    if (tagSignalsDeferred && ! deferred) {
        catchupTagSignals();
    }
    tagSignalsDeferred = deferred;
}


bool
TrainDB::isDeferredTagSignals
()
{
    return tagSignalsDeferred;
}


void
TrainDB::catchupTagSignals
()
{
    if (! tagSignalsDeferred) {
        return;
    }
    if (   deferredTagsAdded.size() > 0
        || deferredTagsDeleted.size() > 0
        || deferredTagsUpdated.size() > 0) {
        emit deferredTagsChanged(deferredTagsAdded, deferredTagsDeleted, deferredTagsUpdated);
        if (   deferredTagsDeleted.size() > 0
            || deferredTagsUpdated.size() > 0) {
            emit dataChanged();
        }
        deferredTagsAdded.clear();
        deferredTagsDeleted.clear();
        deferredTagsUpdated.clear();
    }
}


int
TrainDB::addTag
(const QString &label)
{
    QSqlQuery query(connection());
    query.prepare("INSERT INTO tagstore (label) values (:label)");
    query.bindValue(":label", label);
    if (! query.exec()) {
        qDebug() << "TrainDB::addTag(.) -" << query.lastError() << "/" << query.lastQuery();
    }
    int id = getTagId(label);
    if (id != TAGSTORE_UNDEFINED_ID) {
        if (isDeferredTagSignals()) {
            deferredTagsAdded << id;
        } else {
            emit tagsChanged(id, TAGSTORE_UNDEFINED_ID, TAGSTORE_UNDEFINED_ID);
        }
    }
    return id;
}


bool
TrainDB::updateTag
(int id, const QString &label)
{
    QSqlQuery query(connection());
    query.prepare("UPDATE tagstore "
                  "   SET label = :label "
                  " WHERE id = :id");
    query.bindValue(":id", id);
    query.bindValue(":label", label);
    if (query.exec()) {
        if (isDeferredTagSignals()) {
            deferredTagsUpdated << id;
        } else {
            emit tagsChanged(TAGSTORE_UNDEFINED_ID, TAGSTORE_UNDEFINED_ID, id);
            emit dataChanged();
        }
        return true;
    }
    return false;
}


bool
TrainDB::deleteTag
(int id)
{
    bool ok = true;
    QSqlQuery query(connection());

    query.prepare("DELETE FROM workout_tag WHERE id = :id");
    query.bindValue(":id", id);
    ok &= query.exec();

    query.prepare("DELETE FROM tagstore WHERE id = :id");
    query.bindValue(":id", id);
    ok &= query.exec();

    if (ok) {
        if (isDeferredTagSignals()) {
            deferredTagsDeleted << id;
        } else {
            emit tagsChanged(TAGSTORE_UNDEFINED_ID, id, TAGSTORE_UNDEFINED_ID);
            emit dataChanged();
        }
    }

    return ok;
}


bool
TrainDB::deleteTag
(const QString &label)
{
    bool ok = true;
    QSqlQuery query(connection());

    query.prepare("DELETE "
                  "  FROM workout_tag "
                  " WHERE id IN (SELECT id "
                  "                FROM tagstore "
                  "               WHERE label = :label)");
    query.bindValue(":label", label);
    ok &= query.exec();

    query.prepare("DELETE FROM tagstore WHERE label = :label");
    query.bindValue(":label", label);
    ok &= query.exec();

    return ok;
}


int
TrainDB::getTagId
(const QString &label) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT id FROM tagstore WHERE label = :label");
    query.bindValue(":label", label);
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return TAGSTORE_UNDEFINED_ID;
}


QString
TrainDB::getTagLabel
(int id) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT label FROM tagstore WHERE id = :id");
    query.bindValue(":id", id);
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return "";
}


int
TrainDB::countTagUsage
(int id) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT count(*) FROM workout_tag WHERE id = :id");
    query.bindValue(":id", id);
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}


bool
TrainDB::hasTag
(int id) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT label FROM tagstore WHERE id = :id");
    query.bindValue(":id", id);
    return query.exec() && query.next();
}


bool
TrainDB::hasTag
(const QString &label) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT id FROM tagstore WHERE label = :label");
    query.bindValue(":label", label);
    return query.exec() && query.next();
}


QList<TagStore::Tag>
TrainDB::getTags
() const
{
    QList<TagStore::Tag> ret;
    QSqlQuery query(connection());
    query.prepare("SELECT id, label FROM tagstore");
    if (query.exec()) {
        while (query.next()) {
            ret << TagStore::Tag(query.value(0).toInt(), query.value(1).toString());
        }
    }
    return ret;
}


QList<int>
TrainDB::getTagIds
() const
{
    QList<int> ret;
    QSqlQuery query(connection());
    query.prepare("SELECT id FROM tagstore");
    if (query.exec()) {
        while (query.next()) {
            ret << query.value(0).toInt();
        }
    }
    return ret;
}


QStringList
TrainDB::getTagLabels
() const
{
    QStringList ret;
    QSqlQuery query(connection());
    query.prepare("SELECT label FROM tagstore");
    if (query.exec()) {
        while (query.next()) {
            ret << query.value(0).toString();
        }
    }
    return ret;
}


QStringList
TrainDB::getTagLabels
(const QList<int> ids) const
{
    QStringList ret;
    for (const auto &i : getTagIds()) {
        if (ids.contains(i)) {
            ret << getTagLabel(i);
        }
    }
    return ret;
}

QStringList
TrainDB::getWorkouts
() const
{
    QStringList ret;
    QSqlQuery query(connection());
    query.prepare("SELECT filepath FROM workout WHERE source IS NOT 'gcdefault' AND type IS NOT 'code'");
    if (query.exec()) {
        while (query.next()) {
            ret << query.value(0).toString();
        }
    }
    return ret;
}


QHash<QString, QString>
TrainDB::getWorkoutHashes
() const
{
    QHash<QString, QString> ret;
    QSqlQuery query(connection());
    query.prepare("SELECT filepath FROM workout WHERE source IS NOT 'gcdefault' AND type IS NOT 'code'");
    if (query.exec()) {
        while (query.next()) {
            QFile file(query.value(0).toString());
            if (! file.open(QIODevice::ReadOnly)) {
                continue;
            }
            QCryptographicHash hash(QCryptographicHash::Md5);
            while (! file.atEnd()) {
                hash.addData(file.read(8192));
            }
            ret.insert(hash.result().toHex(), query.value(0).toString());
        }
    }
    return ret;
}


///////////////////////// Helpers for Taggable / Workout

bool
TrainDB::workoutHasTag
(const QString &filepath, int id) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT COUNT(*) FROM workout_tag WHERE filepath = :filepath AND id = :id");
    query.bindValue(":filepath", filepath);
    query.bindValue(":id", id);

    if (query.exec() && query.next()) {
        return query.value(0).toInt() > 0;
    }
    return false;
}


void
TrainDB::workoutAddTag
(const QString &filepath, int id)
{
    QSqlQuery query(connection());
    query.prepare("INSERT OR REPLACE INTO workout_tag (filepath, id) values (:filepath, :id)");
    query.bindValue(":filepath", filepath);
    query.bindValue(":id", id);
    if (query.exec()) {
        emit dataChanged();
    } else {
        qDebug() << "TrainDB::workoutAddTag(.) -" << query.lastError() << "/" << query.lastQuery();
    }
}


void
TrainDB::workoutRemoveTag
(const QString &filepath, int id)
{
    QSqlQuery query(connection());
    query.prepare("DELETE FROM workout_tag WHERE filepath = :filepath AND id = :id");
    query.bindValue(":filepath", filepath);
    query.bindValue(":id", id);
    if (query.exec()) {
        emit dataChanged();
    } else {
        qDebug() << "TrainDB::workoutRemoveTag(.) -" << query.lastError() << "/" << query.lastQuery();
    }
}


void
TrainDB::workoutClearTags
(const QString &filepath)
{
    QSqlQuery query(connection());
    query.prepare("DELETE FROM workout_tag WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    if (query.exec()) {
        emit dataChanged();
    } else {
        qDebug() << "TrainDB::workoutClearTags(.) -" << query.lastError() << "/" << query.lastQuery();
    }
}


QList<int>
TrainDB::workoutGetTagIds
(const QString &filepath) const
{
    QList<int> ret;
    QSqlQuery query(connection());
    query.prepare("  SELECT wt.id "
                  "    FROM workout_tag wt "
                  "         INNER JOIN tagstore ts "
                  "                 ON ts.id = wt.id "
                  "   WHERE wt.filepath = :filepath "
                  "ORDER BY ts.label COLLATE NOCASE");
    query.bindValue(":filepath", filepath);
    if (query.exec()) {
        while (query.next()) {
            ret << query.value(0).toInt();
        }
    }
    return ret;
}


///////////////////////// Helpers for DB-upgrade

TrainDB::LegacyMigrationPlan
TrainDB::legacyMigrationPlan
() const
{
    LegacyMigrationPlan plan;
    if (currentSchemaStatus != SchemaStatus::migrationReady
        || databaseState() != DatabaseState::migrationReady) {
        return plan;
    }

    auto readPaths = [this](const QString &sql,
                            QStringList &paths,
                            const QSet<QString> &ignoredPaths) {
        QSqlQuery query(connection());
        if (!query.exec(sql)) {
            return false;
        }
        while (query.next()) {
            if (query.value(0).isNull()) {
                return false;
            }
            const QString path = query.value(0).toString();
            if (ignoredPaths.contains(path)) {
                continue;
            }
            if (path.isEmpty() || paths.contains(path)) {
                return false;
            }
            paths.append(path);
        }
        paths.sort();
        return true;
    };

    if (!readPaths(QStringLiteral(
                       "SELECT filepath FROM workouts ORDER BY filepath"),
                   plan.workouts,
                   QSet<QString>{QStringLiteral("//1"), QStringLiteral("//2")})
        || !readPaths(QStringLiteral(
                          "SELECT filepath FROM videos ORDER BY filepath"),
                      plan.videos,
                      QSet<QString>{})
        || !readPaths(QStringLiteral(
                          "SELECT filepath FROM videosyncs ORDER BY filepath"),
                      plan.videoSyncs,
                      QSet<QString>{QStringLiteral("//1")})) {
        return {};
    }

    plan.valid = true;
    return plan;
}


QStringList
TrainDB::getMigrateableWorkoutPaths
() const
{
    const LegacyMigrationPlan plan = legacyMigrationPlan();
    return plan.valid ? plan.workouts : QStringList();
}


QStringList
TrainDB::getMigrateableVideoPaths
() const
{
    const LegacyMigrationPlan plan = legacyMigrationPlan();
    return plan.valid ? plan.videos : QStringList();
}


QStringList
TrainDB::getMigrateableVideoSyncPaths
() const
{
    const LegacyMigrationPlan plan = legacyMigrationPlan();
    return plan.valid ? plan.videoSyncs : QStringList();
}


bool
TrainDB::dropLegacyTables
() const
{
    qWarning() << "TrainDB: refusing to drop legacy tables without a verified import";
    return false;
}


bool
TrainDB::finalizeLegacyMigration
(const LegacyMigrationPlan &plan, const LibraryImportResult &result) const
{
    if (currentSchemaStatus != SchemaStatus::migrationReady || !plan.valid) {
        return false;
    }
    if (!plan.isEmpty()
        && !result.importedAll(plan.workouts, plan.videos, plan.videoSyncs)) {
        return false;
    }

    QSqlDatabase database = connection();
    if (!database.transaction()) {
        return false;
    }

    auto samePaths = [](QStringList left, QStringList right) {
        left.sort();
        right.sort();
        return left == right;
    };
    const LegacyMigrationPlan currentPlan = legacyMigrationPlan();
    bool ok = currentPlan.valid
        && samePaths(currentPlan.workouts, plan.workouts)
        && samePaths(currentPlan.videos, plan.videos)
        && samePaths(currentPlan.videoSyncs, plan.videoSyncs);

    auto importedRowsExist = [this](const QString &table,
                                    const QStringList &sources,
                                    const QHash<QString, QString> &imports,
                                    bool exactPath) {
        QSet<QString> destinations;
        QSqlQuery query(connection());
        query.prepare(QStringLiteral(
            "SELECT 1 FROM %1 WHERE filepath = :filepath").arg(table));
        for (const QString &source : sources) {
            const QString destination = imports.value(source);
            if (destination.isEmpty() || destinations.contains(destination)) {
                return false;
            }
            if ((exactPath && destination != source)
                || (!exactPath
                    && QFileInfo(destination).fileName()
                        != QFileInfo(source).fileName())) {
                return false;
            }
            destinations.insert(destination);
            query.bindValue(QStringLiteral(":filepath"), destination);
            if (!query.exec() || !query.next()) {
                return false;
            }
            query.finish();
        }
        return true;
    };

    if (ok && !plan.isEmpty()) {
        ok = importedRowsExist(QStringLiteral(TABLE_WORKOUT),
                               plan.workouts, result.importedWorkouts, false)
          && importedRowsExist(QStringLiteral(TABLE_VIDEO),
                               plan.videos, result.importedVideos, true)
          && importedRowsExist(QStringLiteral(TABLE_VIDEOSYNC),
                               plan.videoSyncs, result.importedVideoSyncs, false);
    }
    if (ok) {
        ok = dropTable(QStringLiteral("workouts"), true)
          && dropTable(QStringLiteral("videos"), true)
          && dropTable(QStringLiteral("videosyncs"), true);
    }
    if (ok) {
        ok = databaseState() == DatabaseState::current;
    }
    if (ok && database.commit()) {
        currentSchemaStatus = SchemaStatus::current;
        return true;
    }

    database.rollback();
    return false;
}


/////////////////////////


bool
TrainDB::createDatabase
() const
{
    QSqlDatabase database = connection();
    const DatabaseState state = databaseState();
    if (state == DatabaseState::current) {
        currentSchemaStatus = SchemaStatus::current;
        return true;
    }
    if (state == DatabaseState::migrationReady) {
        currentSchemaStatus = SchemaStatus::migrationReady;
        return true;
    }
    if (state == DatabaseState::invalid || state == DatabaseState::readError) {
        currentSchemaStatus = state == DatabaseState::invalid
                            ? SchemaStatus::invalid
                            : SchemaStatus::readError;
        qWarning() << "TrainDB: refusing to modify an unrecognized database schema";
        protectDatabaseFromWrites(database);
        return false;
    }

    if (!database.transaction()) {
        currentSchemaStatus = SchemaStatus::initializationFailed;
        qWarning() << "TrainDB: cannot start schema transaction" << database.lastError();
        protectDatabaseFromWrites(database);
        return false;
    }

    bool ok = true;
    if (state == DatabaseState::empty) {
        ok = createTable(TABLE_VERSION, FIELDS_VERSION, false) == 1;
    }
    if (ok) {
        ok = createAllDataTables();
    }

    DatabaseState finalState = DatabaseState::readError;
    if (ok) {
        finalState = databaseState();
        ok = finalState == DatabaseState::current
          || finalState == DatabaseState::migrationReady;
    }
    if (ok && database.commit()) {
        currentSchemaStatus = finalState == DatabaseState::current
                            ? SchemaStatus::current
                            : SchemaStatus::migrationReady;
        return true;
    }

    database.rollback();
    currentSchemaStatus = SchemaStatus::initializationFailed;
    protectDatabaseFromWrites(database);
    qWarning() << "TrainDB: schema initialization failed and was rolled back";
    return false;
}


TrainDB::DatabaseState
TrainDB::databaseState
() const
{
    struct TableLayout {
        QStringList columns;
        QSet<QString> requiredNotNull;
        QStringList primaryKey;
    };

    QSqlDatabase database = connection();
    QHash<QString, QString> objectTypes;
    QSqlQuery objects(database);
    if (!objects.exec("SELECT type, name FROM sqlite_master "
                      "WHERE name NOT LIKE 'sqlite_%'")) {
        return DatabaseState::readError;
    }
    while (objects.next()) {
        objectTypes.insert(objects.value(1).toString(), objects.value(0).toString());
    }
    if (objectTypes.isEmpty()) {
        return DatabaseState::empty;
    }

    const QString versionTable = QStringLiteral(TABLE_VERSION);
    if (!objectTypes.contains(versionTable)) {
        return DatabaseState::invalid;
    }
    if (objectTypes.value(versionTable) != QStringLiteral("table")) {
        return DatabaseState::readError;
    }

    auto checkLayout = [&database](const QString &tableName,
                                   const TableLayout &expected) -> int {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(tableName))) {
            return -1;
        }

        QStringList columns;
        QSet<QString> notNull;
        QHash<int, QString> primaryKeyByPosition;
        while (query.next()) {
            const QString column = query.value(1).toString();
            columns.append(column);
            if (query.value(3).toInt() != 0) {
                notNull.insert(column);
            }
            const int primaryKeyPosition = query.value(5).toInt();
            if (primaryKeyPosition > 0) {
                primaryKeyByPosition.insert(primaryKeyPosition, column);
            }
        }

        QStringList primaryKey;
        for (int position = 1; position <= primaryKeyByPosition.size(); ++position) {
            if (!primaryKeyByPosition.contains(position)) {
                return 0;
            }
            primaryKey.append(primaryKeyByPosition.value(position));
        }

        if (columns != expected.columns || primaryKey != expected.primaryKey) {
            return 0;
        }
        for (const QString &column : expected.requiredNotNull) {
            if (!notNull.contains(column)) {
                return 0;
            }
        }
        return 1;
    };

    TableLayout versionLayout;
    versionLayout.columns = {QStringLiteral("table_name"),
                             QStringLiteral("schema_version"),
                             QStringLiteral("creation_date")};
    versionLayout.primaryKey = {QStringLiteral("table_name")};
    const int versionLayoutResult = checkLayout(versionTable, versionLayout);
    if (versionLayoutResult < 0) {
        return DatabaseState::readError;
    }
    if (versionLayoutResult == 0) {
        return DatabaseState::invalid;
    }

    const QSet<QString> currentTables = {
        QStringLiteral(TABLE_WORKOUT),
        QStringLiteral(TABLE_VIDEO),
        QStringLiteral(TABLE_VIDEOSYNC),
        QStringLiteral(TABLE_TAGSTORE),
        QStringLiteral(TABLE_WORKOUT_TAG)
    };
    const QSet<QString> legacyTables = {
        QStringLiteral("workouts"),
        QStringLiteral("videos"),
        QStringLiteral("videosyncs")
    };

    QSet<QString> currentVersions;
    QSet<QString> legacyVersions;
    QSet<QString> seenVersionRows;
    QSqlQuery versions(database);
    if (!versions.exec("SELECT table_name, schema_version, creation_date FROM version")) {
        return DatabaseState::readError;
    }
    while (versions.next()) {
        if (versions.value(0).isNull()
            || versions.value(1).isNull()
            || versions.value(2).isNull()) {
            return DatabaseState::invalid;
        }

        const QString tableName = versions.value(0).toString();
        bool schemaVersionValid = false;
        bool creationDateValid = false;
        const int schemaVersion = versions.value(1).toInt(&schemaVersionValid);
        versions.value(2).toLongLong(&creationDateValid);
        if (tableName.isEmpty()
            || !schemaVersionValid
            || !creationDateValid
            || seenVersionRows.contains(tableName)) {
            return DatabaseState::invalid;
        }
        seenVersionRows.insert(tableName);

        if (currentTables.contains(tableName) && schemaVersion == TrainDBSchemaVersion) {
            currentVersions.insert(tableName);
        } else if (legacyTables.contains(tableName) && schemaVersion == 1) {
            legacyVersions.insert(tableName);
        } else {
            return DatabaseState::invalid;
        }
    }

    if (seenVersionRows.isEmpty()) {
        return DatabaseState::invalid;
    }

    const bool hasLegacyVersion = !legacyVersions.isEmpty();
    if (hasLegacyVersion) {
        if (legacyVersions != legacyTables) {
            return DatabaseState::invalid;
        }
    } else if (currentVersions != currentTables) {
        return DatabaseState::invalid;
    }

    auto currentLayout = [](const QString &tableName) {
        TableLayout layout;
        if (tableName == QStringLiteral(TABLE_WORKOUT)) {
            layout.columns = {
                QStringLiteral("filepath"), QStringLiteral("type"),
                QStringLiteral("creation_date"), QStringLiteral("source"),
                QStringLiteral("source_id"), QStringLiteral("displayname"),
                QStringLiteral("description"), QStringLiteral("erg_subtype"),
                QStringLiteral("erg_duration"), QStringLiteral("erg_bikestress"),
                QStringLiteral("erg_if"), QStringLiteral("erg_iso_power"),
                QStringLiteral("erg_vi"), QStringLiteral("erg_xp"),
                QStringLiteral("erg_ri"), QStringLiteral("erg_bs"),
                QStringLiteral("erg_svi"), QStringLiteral("erg_min_power"),
                QStringLiteral("erg_max_power"), QStringLiteral("erg_avg_power"),
                QStringLiteral("erg_dominant_zone"), QStringLiteral("erg_num_zones"),
                QStringLiteral("erg_duration_z1"), QStringLiteral("erg_duration_z2"),
                QStringLiteral("erg_duration_z3"), QStringLiteral("erg_duration_z4"),
                QStringLiteral("erg_duration_z5"), QStringLiteral("erg_duration_z6"),
                QStringLiteral("erg_duration_z7"), QStringLiteral("erg_duration_z8"),
                QStringLiteral("erg_duration_z9"), QStringLiteral("erg_duration_z10"),
                QStringLiteral("slp_distance"), QStringLiteral("slp_elevation"),
                QStringLiteral("slp_avg_grade"), QStringLiteral("rating"),
                QStringLiteral("last_run")
            };
            layout.requiredNotNull = {
                QStringLiteral("filepath"), QStringLiteral("type"),
                QStringLiteral("creation_date"), QStringLiteral("displayname")
            };
            layout.primaryKey = {QStringLiteral("filepath")};
        } else if (tableName == QStringLiteral(TABLE_VIDEO)) {
            layout.columns = {
                QStringLiteral("filepath"), QStringLiteral("creation_date"),
                QStringLiteral("displayname")
            };
            layout.requiredNotNull = {
                QStringLiteral("filepath"), QStringLiteral("creation_date"),
                QStringLiteral("displayname")
            };
            layout.primaryKey = {QStringLiteral("filepath")};
        } else if (tableName == QStringLiteral(TABLE_VIDEOSYNC)) {
            layout.columns = {
                QStringLiteral("filepath"), QStringLiteral("creation_date"),
                QStringLiteral("source"), QStringLiteral("displayname")
            };
            layout.requiredNotNull = {
                QStringLiteral("filepath"), QStringLiteral("creation_date"),
                QStringLiteral("displayname")
            };
            layout.primaryKey = {QStringLiteral("filepath")};
        } else if (tableName == QStringLiteral(TABLE_TAGSTORE)) {
            layout.columns = {QStringLiteral("id"), QStringLiteral("label")};
            layout.requiredNotNull = {
                QStringLiteral("id"), QStringLiteral("label")
            };
            layout.primaryKey = {QStringLiteral("id")};
        } else if (tableName == QStringLiteral(TABLE_WORKOUT_TAG)) {
            layout.columns = {QStringLiteral("filepath"), QStringLiteral("id")};
            layout.requiredNotNull = {
                QStringLiteral("filepath"), QStringLiteral("id")
            };
            layout.primaryKey = {
                QStringLiteral("filepath"), QStringLiteral("id")
            };
        }
        return layout;
    };

    auto legacyLayout = [](const QString &tableName) {
        TableLayout layout;
        if (tableName == QStringLiteral("workouts")) {
            layout.columns = {
                QStringLiteral("filepath"), QStringLiteral("filename"),
                QStringLiteral("timestamp"), QStringLiteral("description"),
                QStringLiteral("source"), QStringLiteral("ftp"),
                QStringLiteral("length"), QStringLiteral("coggan_tss"),
                QStringLiteral("coggan_if"), QStringLiteral("elevation"),
                QStringLiteral("grade")
            };
        } else if (tableName == QStringLiteral("videos")) {
            layout.columns = {
                QStringLiteral("filepath"), QStringLiteral("filename"),
                QStringLiteral("timestamp"), QStringLiteral("length")
            };
        } else if (tableName == QStringLiteral("videosyncs")) {
            layout.columns = {
                QStringLiteral("filepath"), QStringLiteral("filename")
            };
        }
        layout.primaryKey = {QStringLiteral("filepath")};
        return layout;
    };

    for (const QString &tableName : currentTables) {
        if (objectTypes.contains(tableName)
            && objectTypes.value(tableName) != QStringLiteral("table")) {
            return DatabaseState::invalid;
        }
        const bool exists = objectTypes.contains(tableName);
        const bool hasVersion = currentVersions.contains(tableName);
        if (exists != hasVersion) {
            return DatabaseState::invalid;
        }
        if (exists) {
            const int result = checkLayout(tableName, currentLayout(tableName));
            if (result < 0) {
                return DatabaseState::readError;
            }
            if (result == 0) {
                return DatabaseState::invalid;
            }
        }
    }

    for (const QString &tableName : legacyTables) {
        if (objectTypes.contains(tableName)
            && objectTypes.value(tableName) != QStringLiteral("table")) {
            return DatabaseState::invalid;
        }
        const bool exists = objectTypes.contains(tableName);
        const bool hasVersion = legacyVersions.contains(tableName);
        if (exists != hasVersion) {
            return DatabaseState::invalid;
        }
        if (exists) {
            const int result = checkLayout(tableName, legacyLayout(tableName));
            if (result < 0) {
                return DatabaseState::readError;
            }
            if (result == 0) {
                return DatabaseState::invalid;
            }
        }
    }

    if (!hasLegacyVersion) {
        return DatabaseState::current;
    }
    return currentVersions == currentTables ? DatabaseState::migrationReady
                                            : DatabaseState::upgradeRequired;
}


bool
TrainDB::checkDBVersion
() const
{
    return createDatabase();
}


TrainDB::SchemaStatus
TrainDB::schemaStatus
() const
{
    return currentSchemaStatus;
}


bool
TrainDB::needsUpgrade
() const
{
    return currentSchemaStatus == SchemaStatus::migrationReady;
}


int
TrainDB::getCount
() const
{
    // how many workouts are there?
    QSqlQuery query("SELECT COUNT(*) FROM workout", connection());

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}


bool
TrainDB::startLUW
()
{
    return connection().transaction();
}


bool
TrainDB::endLUW
()
{
    const bool committed = connection().commit();
    if (committed) {
        emit dataChanged();
    }
    return committed;
}


void
TrainDB::rollbackLUW
()
{
    connection().rollback();
}


QAbstractTableModel*
TrainDB::getWorkoutModel
() const
{
    QSqlQuery query(connection());
    query.prepare("SELECT w.displayname, w.type || '_' || UPPER(w.displayname) AS _sortdummy, "
                  "       w.displayname || ' ' || IFNULL(w.description, '') || ' ' || w.type || ' ' || IFNULL(w.erg_subtype, '') || ' ' || IFNULL(w.source, '') || ' ' || IFNULL(t.tag_labels, '') AS _fulltext, "
                  "       w.description, w.filepath, w.type, w.creation_date, "
                  "       w.erg_duration, "
                  "       w.erg_bikestress, w.erg_if, w.erg_iso_power, w.erg_vi, w.erg_xp, w.erg_ri, w.erg_bs, w.erg_svi, "
                  "       w.erg_min_power, w.erg_max_power, w.erg_avg_power, "
                  "       w.erg_dominant_zone, w.erg_num_zones, "
                  "       w.erg_duration_z1, w.erg_duration_z1 * w.erg_duration / 100000 AS _erg_duration_z1_secs, "
                  "       w.erg_duration_z2, w.erg_duration_z2 * w.erg_duration / 100000 AS _erg_duration_z2_secs, "
                  "       w.erg_duration_z3, w.erg_duration_z3 * w.erg_duration / 100000 AS _erg_duration_z3_secs, "
                  "       w.erg_duration_z4, w.erg_duration_z4 * w.erg_duration / 100000 AS _erg_duration_z4_secs, "
                  "       w.erg_duration_z5, w.erg_duration_z5 * w.erg_duration / 100000 AS _erg_duration_z5_secs, "
                  "       w.erg_duration_z6, w.erg_duration_z6 * w.erg_duration / 100000 AS _erg_duration_z6_secs, "
                  "       w.erg_duration_z7, w.erg_duration_z7 * w.erg_duration / 100000 AS _erg_duration_z7_secs, "
                  "       w.erg_duration_z8, w.erg_duration_z8 * w.erg_duration / 100000 AS _erg_duration_z8_secs, "
                  "       w.erg_duration_z9, w.erg_duration_z9 * w.erg_duration / 100000 AS _erg_duration_z9_secs, "
                  "       w.erg_duration_z10, w.erg_duration_z10 * w.erg_duration / 100000 AS _erg_duration_z10_secs, "
                  "       w.slp_distance, w.slp_elevation, w.slp_avg_grade, "
                  "       IFNULL(w.rating, 0) AS _rating, w.last_run, t.tag_labels "
                  "  FROM workout w "
                  "       LEFT JOIN (SELECT workout_tag.filepath, GROUP_CONCAT(tagstore.label, ' ') AS tag_labels "
                  "                    FROM workout_tag, tagstore "
                  "                   WHERE workout_tag.id = tagstore.id "
                  "                   GROUP BY workout_tag.filepath) t "
                  "              ON w.filepath = t.filepath");

    query.exec();
    QSqlQueryModel *model = new QSqlQueryModel();
    model->setQuery(std::move(query));
    while (model->canFetchMore(QModelIndex())) {
        model->fetchMore(QModelIndex());
    }
    return model;
}


QAbstractTableModel*
TrainDB::getVideoModel
() const
{
    QSqlTableModel *model = new QSqlTableModel(nullptr, connection());
    model->setTable(TABLE_VIDEO);
    model->setEditStrategy(QSqlTableModel::OnManualSubmit);
    model->select();
    while (model->canFetchMore(QModelIndex())) {
        model->fetchMore(QModelIndex());
    }
    return model;
}


QAbstractTableModel*
TrainDB::getVideoSyncModel
() const
{
    QSqlTableModel *model = new QSqlTableModel(nullptr, connection());
    model->setTable(TABLE_VIDEOSYNC);
    model->setEditStrategy(QSqlTableModel::OnManualSubmit);
    model->select();
    while (model->canFetchMore(QModelIndex())) {
        model->fetchMore(QModelIndex());
    }
    return model;
}


int
TrainDB::getDBVersion
() const
{
    int schema_version = -1;

    // can we get a version number?
    QSqlQuery query("SELECT max(schema_version) FROM version", connection());
    if (query.exec() && query.first()) {
        schema_version = query.value(0).toInt();
    }
    return schema_version;
}


/*----------------------------------------------------------------------
 * CRUD routines
 *----------------------------------------------------------------------*/


bool
TrainDB::hasWorkout
(QString filepath) const
{
    return hasItem(filepath, "workout");
}


bool
TrainDB::deleteWorkout
(QString filepath) const
{
    bool ok = true;
    QSqlQuery query(connection());

    query.prepare("DELETE FROM workout_tag WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    ok &= query.exec();

    query.prepare("DELETE FROM workout WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    ok &= query.exec();

    return ok;
}


bool
TrainDB::rateWorkout
(QString filepath, int rating)
{
    QSqlQuery query(connection());

    query.prepare("UPDATE workout "
                  "   SET rating = :rating "
                  " WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    query.bindValue(":rating", rating);
    bool ret = query.exec();
    if (ret) {
        emit dataChanged();
    }
    return ret;
}


bool
TrainDB::lastWorkout
(QString filepath)
{
    QSqlQuery query(connection());

    query.prepare("UPDATE workout "
                  "   SET last_run = :last_run "
                  " WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    query.bindValue(":last_run", QDateTime::currentDateTime().toSecsSinceEpoch());
    bool ret = query.exec();
    if (ret) {
        emit dataChanged();
    }
    return ret;
}


WorkoutUserInfo
TrainDB::getWorkoutUserInfo
(QString filepath) const
{
    WorkoutUserInfo ret;
    QSqlQuery query(connection());
    query.prepare("SELECT rating, last_run "
                  "  FROM workout "
                  " WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    if (query.exec() && query.next()) {
        ret.rating = query.value(0).toInt();
        ret.lastRun = query.value(1).toLongLong();
    }
    return ret;
}


bool
TrainDB::importWorkout
(QString filepath, const ErgFileBase &ergFileBase, ImportMode importMode) const
{
    if (importMode == ImportMode::replace) {
        deleteWorkout(filepath);
    }

    QSqlQuery query(connection());

    bool ok = false;
    if (   importMode == ImportMode::replace
        || importMode == ImportMode::insert
        || importMode == ImportMode::insertOrUpdate) {
        query.prepare("INSERT "
                      "  INTO workout "
                      "       (filepath, type, source, source_id, displayname, description, "
                      "        erg_subtype, erg_duration, erg_bikestress, erg_if, "
                      "        erg_iso_power, erg_vi, erg_xp, erg_ri, erg_bs, erg_svi, "
                      "        erg_min_power, erg_max_power, erg_avg_power, "
                      "        erg_dominant_zone, erg_num_zones, "
                      "        erg_duration_z1, erg_duration_z2, erg_duration_z3, erg_duration_z4, erg_duration_z5, "
                      "        erg_duration_z6, erg_duration_z7, erg_duration_z8, erg_duration_z9, erg_duration_z10, "
                      "        slp_distance, slp_elevation, slp_avg_grade) "
                      "VALUES (:filepath, :type, :source, :source_id, :displayname, :description, "
                      "        :erg_subtype, :erg_duration,:erg_bikestress, :erg_if, "
                      "        :erg_iso_power, :erg_vi, :erg_xp, :erg_ri, :erg_bs, :erg_svi, "
                      "        :erg_min_power, :erg_max_power, :erg_avg_power, "
                      "        :erg_dominant_zone, :erg_num_zones, "
                      "        :erg_duration_z1, :erg_duration_z2, :erg_duration_z3, :erg_duration_z4, :erg_duration_z5, "
                      "        :erg_duration_z6, :erg_duration_z7, :erg_duration_z8, :erg_duration_z9, :erg_duration_z10, "
                      "        :slp_distance, :slp_elevation, :slp_avg_grade)");
        bindWorkout(query, filepath, ergFileBase);
        ok = query.exec();
    }
    if (   hasWorkout(filepath)
        && (   (! ok && importMode == ImportMode::insertOrUpdate)
            || importMode == ImportMode::update)) {
        query.prepare("UPDATE workout "
                      "   SET type = :type, source = :source, source_id = :source_id, displayname = :displayname, description = :description, "
                      "       erg_subtype = :erg_subtype, erg_duration = :erg_duration, erg_bikestress = :erg_bikestress, erg_if = :erg_if, "
                      "       erg_iso_power = :erg_iso_power, erg_vi = :erg_vi, erg_xp = :erg_xp, erg_ri = :erg_ri, erg_bs = :erg_bs, erg_svi = :erg_svi, "
                      "       erg_min_power = :erg_min_power, erg_max_power = :erg_max_power, erg_avg_power = erg_avg_power, "
                      "       erg_dominant_zone = :erg_dominant_zone, erg_num_zones = :erg_num_zones, "
                      "       erg_duration_z1 = :erg_duration_z1, erg_duration_z2 = :erg_duration_z2, "
                      "       erg_duration_z3 = :erg_duration_z3, erg_duration_z4 = :erg_duration_z4, "
                      "       erg_duration_z5 = :erg_duration_z5, erg_duration_z6 = :erg_duration_z6, "
                      "       erg_duration_z7 = :erg_duration_z7, erg_duration_z8 = :erg_duration_z8, "
                      "       erg_duration_z9 = :erg_duration_z9, erg_duration_z10 = :erg_duration_z10, "
                      "       slp_distance = :slp_distance, slp_elevation = :slp_elevation, slp_avg_grade = :slp_avg_grade "
                      " WHERE filepath = :filepath");
        bindWorkout(query, filepath, ergFileBase);
        ok = query.exec();
    }

    return ok;
}


bool
TrainDB::hasVideoSync
(QString filepath) const
{
    return hasItem(filepath, "videosync");
}


bool
TrainDB::deleteVideoSync
(QString filepath) const
{
    QSqlQuery query(connection());

    query.prepare("DELETE FROM videosync WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    return query.exec();
}


bool
TrainDB::importVideoSync
(QString filepath, const VideoSyncFileBase &videoSyncFileBase, ImportMode importMode) const
{
    if (importMode == ImportMode::replace) {
        deleteVideoSync(filepath);
    }

    QSqlQuery query(connection());

    bool ok = false;
    if (   importMode == ImportMode::replace
        || importMode == ImportMode::insert
        || importMode == ImportMode::insertOrUpdate) {
        query.prepare("INSERT "
                      "  INTO videosync "
                      "       (filepath, source, displayname)"
                      "VALUES (:filepath, :source, :displayname)");
        query.bindValue(":filepath", filepath);
        if (! bindIfValue(query, ":displayname", videoSyncFileBase.name())) {
            bindIfValue(query, ":displayname", QFileInfo(filepath).baseName());
        }
        bindIfValue(query, ":source", videoSyncFileBase.source());
        ok = query.exec();
    }
    if (   hasVideoSync(filepath)
        && (   (! ok && importMode == ImportMode::insertOrUpdate)
            || importMode == ImportMode::update)) {
        query.prepare("UPDATE videosync "
                      "   SET source = :source, displayname = :displayname "
                      " WHERE filepath = :filepath");
        query.bindValue(":filepath", filepath);
        if (! bindIfValue(query, ":displayname", videoSyncFileBase.name())) {
            bindIfValue(query, ":displayname", QFileInfo(filepath).baseName());
        }
        bindIfValue(query, ":source", videoSyncFileBase.source());
        ok = query.exec();
    }

    return ok;
}


bool
TrainDB::hasVideo
(QString filepath) const
{
    return hasItem(filepath, "video");
}


bool
TrainDB::deleteVideo
(QString filepath) const
{
    QSqlQuery query(connection());

    query.prepare("DELETE FROM video WHERE filepath = :filepath");
    query.bindValue(":filepath", filepath);
    return query.exec();
}


bool
TrainDB::importVideo
(QString filepath, ImportMode importMode) const
{
    if (importMode == ImportMode::replace) {
        deleteVideo(filepath);
    }

    QSqlQuery query(connection());

    bool ok = false;
    if (   importMode == ImportMode::replace
        || importMode == ImportMode::insert
        || importMode == ImportMode::insertOrUpdate) {
        query.prepare("INSERT "
                      "  INTO video "
                      "       (filepath, displayname)"
                      "VALUES (:filepath, :displayname)");
        query.bindValue(":filepath", filepath);
        query.bindValue(":displayname", QFileInfo(filepath).baseName());
        ok = query.exec();
    }
    if (   hasVideo(filepath)
        && (   (! ok && importMode == ImportMode::insertOrUpdate)
            || importMode == ImportMode::update)) {
        query.prepare("UPDATE video "
                      "   SET displayname = :displayname "
                      " WHERE filepath = :filepath");
        query.bindValue(":filepath", filepath);
        query.bindValue(":displayname", QFileInfo(filepath).baseName());
        ok = query.exec();
    }

    return ok;
}


bool
TrainDB::createDefaultEntriesWorkout
() const
{
    bool rc = true;

    ErgFileBase efb;
    efb.name(tr("Manual Erg Mode"));
    efb.format(ErgFileFormat::code);
    efb.source("gcdefault");
    rc &= importWorkout("//1", efb);

    efb.name(tr("Manual Slope Mode"));
    efb.format(ErgFileFormat::code);
    efb.source("gcdefault");
    rc &= importWorkout("//2", efb);

    return rc;
}


bool
TrainDB::hasItem
(QString filepath, QString table) const
{
    int hasItem = false;

    QSqlQuery query(connection());
    query.prepare(QString("SELECT count(*) FROM %1 WHERE filepath = :filepath").arg(table));
    query.bindValue(":filepath", filepath);
    if (query.exec() && query.first()) {
        hasItem = query.value(0).toInt() > 0;
    }
    return hasItem;
}


bool
TrainDB::upgradeDefaultEntriesWorkout
()
{
    // set texts starting with " " in upgrade - since due to same translation errors the " " was lost e.g. in German
    QSqlQuery query(connection());

    // adding a space at the front of string to make manual mode always
    // appear first in a sorted list is a bit of a hack, but works ok
    QString manualErg = QString("UPDATE workouts SET filename = \"%1\" WHERE filepath = \"//1\";")
            .arg(" " + tr("Manual Erg Mode")); // keep the SPACE separate so that translation cannot remove it
    bool rc = query.exec(manualErg);

    QString manualCrs = QString("UPDATE workouts SET filename = \"%1\" WHERE filepath = \"//2\";")
            .arg(" " + tr("Manual Slope Mode")); // keep the SPACE separate so that translation cannot remove it
    rc &= query.exec(manualCrs);

    return rc;
}


bool
TrainDB::createDefaultEntriesVideo
() const
{
    // insert the 'DVD' record for playing currently loaded DVD
    // need to resolve DVD playback in v3.1, there is an open feature request for this.
    // rc = query.exec("INSERT INTO videos (filepath, filename) values (\"\", \"DVD\");");
    return true;
}


bool
TrainDB::createDefaultEntriesVideoSync
() const
{
    bool rc = true;

    // adding a space at the front of string to make "None" always
    // appear first in a sorted list is a bit of a hack, but works ok
    VideoSyncFileBase vsfb;
    vsfb.name(" " + tr("None")); // keep the SPACE separate so that translation cannot remove it
    vsfb.source("gcdefault");
    rc &= importVideoSync("//1", vsfb);

    return rc;

}


bool
TrainDB::createDefaultEntriesTagStore
() const
{
    return true;
}


bool
TrainDB::createDefaultEntriesWorkoutTags
() const
{
    return true;
}


bool
TrainDB::createAllDataTables
() const
{
    bool ok = true;
    int ret;
    ok &= (ret = createTable(TABLE_WORKOUT, FIELDS_WORKOUT)) != -1;
    if (ret > 0) {
        ok &= createDefaultEntriesWorkout();
    }
    ok &= (ret = createTable(TABLE_VIDEO, FIELDS_VIDEO)) != -1;
    if (ret > 0) {
        ok &= createDefaultEntriesVideo();
    }
    ok &= (ret = createTable(TABLE_VIDEOSYNC, FIELDS_VIDEOSYNC)) != -1;
    if (ret > 0) {
        ok &= createDefaultEntriesVideoSync();
    }
    ok &= (ret = createTable(TABLE_TAGSTORE, FIELDS_TAGSTORE)) != -1;
    if (ret > 0) {
        ok &= createDefaultEntriesTagStore();
    }
    ok &= (ret = createTable(TABLE_WORKOUT_TAG, FIELDS_WORKOUT_TAG)) != -1;
    if (ret > 0) {
        ok &= createDefaultEntriesWorkoutTags();
    }
    return ok;
}


bool
TrainDB::dropAllDataTables
() const
{
    bool ok = dropTable(TABLE_WORKOUT);
    ok &= dropTable(TABLE_VIDEO);
    ok &= dropTable(TABLE_VIDEOSYNC);
    ok &= dropTable(TABLE_TAGSTORE);
    ok &= dropTable(TABLE_WORKOUT_TAG);
    return ok;
}

// Keep tables that can hold user date: ratings, tags, etc: TABLE_WORKOUT, TABLE_TAGSTORE and TABLE_WORKOUT_TAG
bool
TrainDB::dropTablesButUserDataTables
() const
{
    bool ok = dropTable(TABLE_VIDEO);
    ok &= dropTable(TABLE_VIDEOSYNC);
    return ok;
}



bool
TrainDB::dropTable
(QString tableName, bool hasVersionEntry) const
{
    bool ok = true;
    if (hasVersionEntry) {
        QSqlQuery queryDelete(connection());
        queryDelete.prepare("DELETE FROM version WHERE table_name = :tablename");
        queryDelete.bindValue(":tablename", tableName);
        ok = queryDelete.exec();
    }
    if (ok) {
        QSqlQuery queryDrop(QString("DROP TABLE IF EXISTS %1").arg(tableName), connection());
        ok = queryDrop.exec();
    }
    return ok;
}


// 1. Check if the table exists (query sqlite_master) - if found: No action required: exit
// 2. Create table
// 3. Create default entries
// 4. Update version of table
int
TrainDB::createTable
(QString tableName, QString createBody, bool hasVersionEntry, bool force) const
{
    int ret = 0;
    bool ok = true;
    int version = 0;

    if (! force && hasVersionEntry) {
        if ((version = getTableVersion(tableName)) == -2) {
            return -1;
        }
    }
    if (force) {
        dropTable(tableName);
    }
    if (force || ! hasVersionEntry || (hasVersionEntry && version < TrainDBSchemaVersion)) {
        QSqlQuery query(connection());
        if (! (ok = query.exec(QString("CREATE TABLE %1 (%2)").arg(tableName, createBody)))) {
            qDebug() << "TrainDB::createTable(.) -" << query.lastError() << "/" << query.lastQuery();
        }
        if (ok && hasVersionEntry) {
            ok = updateTableVersion(tableName);
        }
        ret = ok ? 1 : -1;
    } else {
        ret = 0;
    }
    return ret;
}


bool
TrainDB::updateTableVersion
(QString tableName) const
{
    QSqlQuery query(connection());
    query.prepare("INSERT OR REPLACE INTO version (table_name, schema_version, creation_date) VALUES (:tablename, :schemaversion, :creationdate)");
    query.bindValue(":tablename", tableName);
    query.bindValue(":schemaversion", TrainDBSchemaVersion);
    query.bindValue(":creationdate", QDateTime::currentDateTime().toSecsSinceEpoch());
    bool ok = query.exec();
    if (! ok) {
        qDebug() << "TrainDB::updateTableVersion(.) -" << query.lastError() << "/" << query.lastQuery();
    }
    return ok;
}


int
TrainDB::getTableVersion
(QString tableName) const
{
    QSqlQuery masterQuery(connection());
    masterQuery.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name = :tablename");
    masterQuery.bindValue(":tablename", tableName);
    if (masterQuery.exec()) {
        if (! masterQuery.next()) {
            return -1;
        }
    } else {
        return -2;
    }

    int version = -1;
    QSqlQuery versionQuery(connection());
    versionQuery.prepare("SELECT schema_version FROM version WHERE table_name = :tablename");
    versionQuery.bindValue(":tablename", tableName);
    if (versionQuery.exec()) {
        if (versionQuery.first()) {
            version = versionQuery.value(0).toInt();
        }
    } else {
        version = -2;
    }
    return version;
}


bool
TrainDB::bindIfValue
(QSqlQuery &query, const QString &placeholder, const QString &value) const
{
    if (value.isEmpty()) {
        return false;
    }
    query.bindValue(placeholder, value);
    return true;
}


bool
TrainDB::bindIfValue
(QSqlQuery &query, const QString &placeholder, double value) const
{
    if (qFuzzyIsNull(value)) {
        return false;
    }
    query.bindValue(placeholder, value);
    return true;
}


bool
TrainDB::bindIfValue
(QSqlQuery &query, const QString &placeholder, qlonglong value) const
{
    if (value == 0) {
        return false;
    }
    query.bindValue(placeholder, value);
    return true;
}


bool
TrainDB::bindIfValue
(QSqlQuery &query, const QString &placeholder, int value) const
{
    if (value == 0) {
        return false;
    }
    query.bindValue(placeholder, value);
    return true;
}


bool
TrainDB::bindIfPredecessor
(QSqlQuery &query, const QString &placeholder, double value, bool predecessor) const
{
    if (! predecessor) {
        return false;
    }
    query.bindValue(placeholder, value);
    return true;
}


void
TrainDB::bindWorkout
(QSqlQuery &query, const QString &filepath, const ErgFileBase &ergFileBase) const
{
    query.bindValue(":filepath", filepath);
    query.bindValue(":type", ergFileBase.typeString());
    bindIfValue(query, ":source", ergFileBase.source());
    bindIfValue(query, ":source_id", ergFileBase.trainerDayId());
    if (! bindIfValue(query, ":displayname", ergFileBase.name().trimmed())) {
        query.bindValue(":displayname", QFileInfo(filepath).baseName());
    }
    bindIfValue(query, ":description", ergFileBase.description());
    if (ergFileBase.hasWatts()) {
        bindIfValue(query, ":erg_subtype", ergFileBase.ergSubTypeString());
        bindIfValue(query, ":erg_duration", qlonglong(ergFileBase.duration()));
        bindIfValue(query, ":erg_bikestress", ergFileBase.bikeStress());
        bindIfValue(query, ":erg_if", ergFileBase.IF());
        bindIfValue(query, ":erg_iso_power", ergFileBase.IsoPower());
        bindIfValue(query, ":erg_vi", ergFileBase.VI());
        bindIfValue(query, ":erg_xp", ergFileBase.XP());
        bindIfValue(query, ":erg_ri", ergFileBase.RI());
        bindIfValue(query, ":erg_bs", ergFileBase.BS());
        bindIfValue(query, ":erg_svi", ergFileBase.SVI());
        query.bindValue(":erg_min_power", ergFileBase.minWatts());
        query.bindValue(":erg_max_power", ergFileBase.maxWatts());
        query.bindValue(":erg_avg_power", ergFileBase.AP());
        query.bindValue(":erg_dominant_zone", ergFileBase.dominantZoneInt());
        query.bindValue(":erg_num_zones", ergFileBase.numZones());
        bindIfPredecessor(query, ":erg_duration_z1", ergFileBase.powerZonePC(ErgFilePowerZone::z1), ergFileBase.numZones() >= 1);
        bindIfPredecessor(query, ":erg_duration_z2", ergFileBase.powerZonePC(ErgFilePowerZone::z2), ergFileBase.numZones() >= 2);
        bindIfPredecessor(query, ":erg_duration_z3", ergFileBase.powerZonePC(ErgFilePowerZone::z3), ergFileBase.numZones() >= 3);
        bindIfPredecessor(query, ":erg_duration_z4", ergFileBase.powerZonePC(ErgFilePowerZone::z4), ergFileBase.numZones() >= 4);
        bindIfPredecessor(query, ":erg_duration_z5", ergFileBase.powerZonePC(ErgFilePowerZone::z5), ergFileBase.numZones() >= 5);
        bindIfPredecessor(query, ":erg_duration_z6", ergFileBase.powerZonePC(ErgFilePowerZone::z6), ergFileBase.numZones() >= 6);
        bindIfPredecessor(query, ":erg_duration_z7", ergFileBase.powerZonePC(ErgFilePowerZone::z7), ergFileBase.numZones() >= 7);
        bindIfPredecessor(query, ":erg_duration_z8", ergFileBase.powerZonePC(ErgFilePowerZone::z8), ergFileBase.numZones() >= 8);
        bindIfPredecessor(query, ":erg_duration_z9", ergFileBase.powerZonePC(ErgFilePowerZone::z9), ergFileBase.numZones() >= 9);
        bindIfPredecessor(query, ":erg_duration_z10", ergFileBase.powerZonePC(ErgFilePowerZone::z10), ergFileBase.numZones() >= 10);
    } else if (ergFileBase.type() == ErgFileType::slp) {
        query.bindValue(":slp_distance", qlonglong(ergFileBase.duration()));
        query.bindValue(":slp_elevation", ergFileBase.ele());
        query.bindValue(":slp_avg_grade", ergFileBase.grade());
    }
}
