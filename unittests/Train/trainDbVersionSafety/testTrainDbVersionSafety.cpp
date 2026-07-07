#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "Library.h"
#include "TrainDB.h"

namespace {

class ScopedDatabase
{
public:
    explicit ScopedDatabase(const QString &path)
        : name(QStringLiteral("train-db-version-safety-%1").arg(++nextId))
        , database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name))
    {
        database.setDatabaseName(path);
        database.open();
    }

    ~ScopedDatabase()
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }

    QSqlDatabase &get() { return database; }

private:
    static int nextId;
    QString name;
    QSqlDatabase database;
};

int ScopedDatabase::nextId = 0;

bool execSql(QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    if (query.exec(sql)) {
        return true;
    }
    qWarning().noquote() << query.lastError().text() << "for" << sql;
    return false;
}

bool initializeCurrentDatabase(const QString &homePath)
{
    TrainDB database{QDir(homePath)};
    return QFile::exists(homePath + QStringLiteral("/trainDB"));
}

bool addCurrentSentinel(const QString &databasePath)
{
    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    return database.isOpen()
        && execSql(database,
                   QStringLiteral("INSERT INTO workout "
                                  "(filepath, type, displayname, rating, last_run) "
                                  "VALUES ('/sentinel.erg', 'erg', 'Sentinel', 5, 123456789)"))
        && execSql(database,
                   QStringLiteral("INSERT INTO tagstore (id, label) "
                                  "VALUES (42, 'Important')"))
        && execSql(database,
                   QStringLiteral("INSERT INTO workout_tag (filepath, id) "
                                  "VALUES ('/sentinel.erg', 42)"))
        && execSql(database,
                   QStringLiteral("CREATE TABLE durable_marker (value TEXT NOT NULL)"))
        && execSql(database,
                   QStringLiteral("INSERT INTO durable_marker VALUES ('keep-me')"));
}

bool addNonUserSentinels(const QString &databasePath)
{
    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    return database.isOpen()
        && execSql(database,
                   QStringLiteral("INSERT INTO video (filepath, displayname) "
                                  "VALUES ('/sentinel.mp4', 'Sentinel video')"))
        && execSql(database,
                   QStringLiteral("INSERT INTO videosync "
                                  "(filepath, source, displayname) "
                                  "VALUES ('/sentinel.rlv', 'user', 'Sentinel sync')"));
}

QByteArray databaseHash(const QString &databasePath)
{
    QFile file(databasePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

bool currentSentinelIsIntact(const QString &databasePath)
{
    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    if (!database.isOpen()) {
        return false;
    }

    QSqlQuery workout(database);
    if (!workout.exec(QStringLiteral("SELECT rating, last_run FROM workout "
                                     "WHERE filepath = '/sentinel.erg'"))
        || !workout.next()
        || workout.value(0).toInt() != 5
        || workout.value(1).toLongLong() != 123456789) {
        return false;
    }

    QSqlQuery tags(database);
    if (!tags.exec(QStringLiteral("SELECT tagstore.label FROM workout_tag "
                                  "JOIN tagstore USING (id) "
                                  "WHERE workout_tag.filepath = '/sentinel.erg'"))
        || !tags.next()
        || tags.value(0).toString() != QStringLiteral("Important")) {
        return false;
    }

    QSqlQuery marker(database);
    return marker.exec(QStringLiteral("SELECT value FROM durable_marker"))
        && marker.next()
        && marker.value(0).toString() == QStringLiteral("keep-me");
}

bool tableExists(QSqlDatabase &database, const QString &tableName)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master "
                                 "WHERE type = 'table' AND name = :name"));
    query.bindValue(QStringLiteral(":name"), tableName);
    return query.exec() && query.next();
}

bool createLegacyVersionOneDatabase(const QString &databasePath)
{
    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    if (!database.isOpen() || !database.transaction()) {
        return false;
    }

    bool ok = execSql(database,
                      QStringLiteral("CREATE TABLE version ("
                                     "table_name varchar primary key, "
                                     "schema_version integer, creation_date date)"));
    ok = ok && execSql(database,
                       QStringLiteral("CREATE TABLE workouts ("
                                      "filepath varchar primary key, filename, timestamp integer, "
                                      "description varchar, source varchar, ftp integer, length integer, "
                                      "coggan_tss integer, coggan_if integer, elevation integer, grade double)"));
    ok = ok && execSql(database,
                       QStringLiteral("CREATE TABLE videos ("
                                      "filepath varchar primary key, filename varchar, "
                                      "timestamp integer, length integer)"));
    ok = ok && execSql(database,
                       QStringLiteral("CREATE TABLE videosyncs ("
                                      "filepath varchar primary key, filename varchar)"));
    ok = ok && execSql(database,
                       QStringLiteral("INSERT INTO version VALUES "
                                      "('workouts', 1, 100), ('videos', 1, 100), "
                                      "('videosyncs', 1, 100)"));
    ok = ok && execSql(database,
                       QStringLiteral("INSERT INTO workouts "
                                      "(filepath, filename, description, source, ftp, length) "
                                      "VALUES ('/legacy.erg', 'Legacy', 'Keep', 'user', 190, 3600)"));
    ok = ok && execSql(database,
                       QStringLiteral("INSERT INTO videos VALUES "
                                      "('/legacy.mp4', 'Legacy video', 100, 3600)"));
    ok = ok && execSql(database,
                       QStringLiteral("INSERT INTO videosyncs VALUES "
                                      "('/legacy.rlv', 'Legacy sync')"));

    if (ok && database.commit()) {
        return true;
    }
    database.rollback();
    return false;
}

bool legacySentinelsAreIntact(const QString &databasePath)
{
    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    QSqlQuery query(database);
    return query.exec(QStringLiteral("SELECT filename, description, source, ftp, length "
                                     "FROM workouts WHERE filepath = '/legacy.erg'"))
        && query.next()
        && query.value(0).toString() == QStringLiteral("Legacy")
        && query.value(1).toString() == QStringLiteral("Keep")
        && query.value(2).toString() == QStringLiteral("user")
        && query.value(3).toInt() == 190
        && query.value(4).toInt() == 3600;
}

bool addSecondLegacyRows(const QString &databasePath)
{
    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    if (!database.isOpen() || !database.transaction()) {
        return false;
    }

    bool ok = execSql(database,
                      QStringLiteral("INSERT INTO workouts "
                                     "(filepath, filename, description, source) "
                                     "VALUES ('/legacy-2.erg', 'Legacy 2', 'Keep 2', 'user')"));
    ok = ok && execSql(database,
                       QStringLiteral("INSERT INTO videos VALUES "
                                      "('/legacy-2.mp4', 'Legacy video 2', 200, 7200)"));
    ok = ok && execSql(database,
                       QStringLiteral("INSERT INTO videosyncs VALUES "
                                      "('/legacy-2.rlv', 'Legacy sync 2')"));
    if (ok && database.commit()) {
        return true;
    }
    database.rollback();
    return false;
}

bool seedImportedRows(const QString &databasePath,
                      const TrainDB::LegacyMigrationPlan &plan,
                      LibraryImportResult &result)
{
    result = LibraryImportResult();
    result.requestedFiles = plan.files();

    ScopedDatabase connection(databasePath);
    QSqlDatabase &database = connection.get();
    if (!database.isOpen() || !database.transaction()) {
        return false;
    }

    QSqlQuery workout(database);
    workout.prepare(QStringLiteral(
        "INSERT INTO workout (filepath, type, displayname) "
        "VALUES (:filepath, 'erg', :displayname)"));
    for (const QString &source : plan.workouts) {
        workout.bindValue(QStringLiteral(":filepath"), source);
        workout.bindValue(QStringLiteral(":displayname"),
                          QFileInfo(source).baseName());
        if (!workout.exec()) {
            database.rollback();
            return false;
        }
        workout.finish();
        result.importedWorkouts.insert(source, source);
    }

    QSqlQuery video(database);
    video.prepare(QStringLiteral(
        "INSERT INTO video (filepath, displayname) "
        "VALUES (:filepath, :displayname)"));
    for (const QString &source : plan.videos) {
        video.bindValue(QStringLiteral(":filepath"), source);
        video.bindValue(QStringLiteral(":displayname"),
                        QFileInfo(source).baseName());
        if (!video.exec()) {
            database.rollback();
            return false;
        }
        video.finish();
        result.importedVideos.insert(source, source);
    }

    QSqlQuery videoSync(database);
    videoSync.prepare(QStringLiteral(
        "INSERT INTO videosync (filepath, source, displayname) "
        "VALUES (:filepath, 'migration-test', :displayname)"));
    for (const QString &source : plan.videoSyncs) {
        videoSync.bindValue(QStringLiteral(":filepath"), source);
        videoSync.bindValue(QStringLiteral(":displayname"),
                            QFileInfo(source).baseName());
        if (!videoSync.exec()) {
            database.rollback();
            return false;
        }
        videoSync.finish();
        result.importedVideoSyncs.insert(source, source);
    }

    if (!database.commit()) {
        database.rollback();
        return false;
    }
    result.completed = true;
    return true;
}

} // namespace

class TestTrainDbVersionSafety : public QObject
{
    Q_OBJECT

private slots:
    void malformedVersionTableFailsClosed();
    void invalidSchemaRejectsSubsequentWrites();
    void missingVersionTableFailsClosed();
    void unreadableVersionViewFailsClosed();
    void unknownSchemaVersionFailsClosed();
    void inconsistentCurrentSchemaFailsClosed();
    void validCurrentDatabaseIsUnchanged();
    void emptyDatabaseIsInitialized();
    void failedUpgradeRollsBackAtomically();
    void lockedDatabaseIsUnchanged();
    void corruptDatabaseIsUnchanged();
    void rebuildAllRollsBackOnFailure();
    void rebuildNonUserTablesRollsBackOnFailure();
    void legacyDropRequiresVerifiedImport();
    void dropLegacyTablesRollsBackOnFailure();
    void finalizeRejectsChangedLegacyPlan();
    void finalizeRequiresEveryImportedRow();
    void finalizeLockedDatabaseIsUnchanged();
    void emptyLegacyMigrationFinalizes();
    void libraryResultGuardsLegacyDrop();
    void uncPathsAreMigrated();
    void nullLegacyPathBlocksPlan();
    void videoSyncImportSupportsMigrationRetry();
    void versionOneUpgradePreservesLegacyTables();
};

void TestTrainDbVersionSafety::malformedVersionTableFailsClosed()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));

    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("DROP TABLE version")));
        QVERIFY(execSql(connection.get(), QStringLiteral("CREATE TABLE version (broken INTEGER)")));
        QVERIFY(execSql(connection.get(), QStringLiteral("INSERT INTO version VALUES (7)")));
    }

    const QByteArray before = databaseHash(path);
    QVERIFY(!before.isEmpty());
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::invalid);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::invalidSchemaRejectsSubsequentWrites()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));

    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("DROP TABLE version")));
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("CREATE TABLE version (broken INTEGER)")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::invalid);
        QVERIFY(!database.deleteWorkout(QStringLiteral("/sentinel.erg")));
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::missingVersionTableFailsClosed()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("DROP TABLE version")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::invalid);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::unreadableVersionViewFailsClosed()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("DROP TABLE version")));
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("CREATE VIEW version AS "
                                       "SELECT * FROM missing_version_source")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::readError);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::unknownSchemaVersionFailsClosed()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("UPDATE version SET schema_version = 99")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::invalid);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::inconsistentCurrentSchemaFailsClosed()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("DROP TABLE video")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::invalid);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
    ScopedDatabase connection(path);
    QVERIFY(!tableExists(connection.get(), QStringLiteral("video")));
}

void TestTrainDbVersionSafety::validCurrentDatabaseIsUnchanged()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::emptyDatabaseIsInitialized()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);
    }

    ScopedDatabase connection(home.path() + QStringLiteral("/trainDB"));
    QSqlDatabase &database = connection.get();
    const QStringList tables = {QStringLiteral("version"), QStringLiteral("workout"),
                                QStringLiteral("video"), QStringLiteral("videosync"),
                                QStringLiteral("tagstore"), QStringLiteral("workout_tag")};
    for (const QString &table : tables) {
        QVERIFY2(tableExists(database, table), qPrintable(table));
    }
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM version WHERE schema_version = 2")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 5);
}

void TestTrainDbVersionSafety::failedUpgradeRollsBackAtomically()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("CREATE TRIGGER reject_v2_version "
                                       "BEFORE INSERT ON version "
                                       "WHEN NEW.schema_version = 2 "
                                       "AND NEW.table_name = 'workout_tag' "
                                       "BEGIN SELECT RAISE(ABORT, 'blocked'); END")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(!database.needsUpgrade());
        QVERIFY(database.schemaStatus()
                == TrainDB::SchemaStatus::initializationFailed);
        const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
        QVERIFY(!plan.valid);
        LibraryImportResult result;
        QVERIFY(!database.finalizeLegacyMigration(plan, result));
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(legacySentinelsAreIntact(path));

    ScopedDatabase connection(path);
    QSqlQuery currentTables(connection.get());
    QVERIFY(currentTables.exec(
        QStringLiteral("SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type = 'table' "
                       "AND name IN ('workout', 'video', 'videosync', "
                       "'tagstore', 'workout_tag')")));
    QVERIFY(currentTables.next());
    QCOMPARE(currentTables.value(0).toInt(), 0);

    QSqlQuery legacyVersions(connection.get());
    QVERIFY(legacyVersions.exec(
        QStringLiteral("SELECT COUNT(*) FROM version WHERE schema_version = 1")));
    QVERIFY(legacyVersions.next());
    QCOMPARE(legacyVersions.value(0).toInt(), 3);
}

void TestTrainDbVersionSafety::lockedDatabaseIsUnchanged()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));

    ScopedDatabase lock(path);
    QVERIFY(execSql(lock.get(), QStringLiteral("BEGIN EXCLUSIVE")));
    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(!database.needsUpgrade());
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::readError);
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(execSql(lock.get(), QStringLiteral("ROLLBACK")));
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::corruptDatabaseIsUnchanged()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray corruptBytes(4096, '\x5a');
    QCOMPARE(file.write(corruptBytes), qint64(corruptBytes.size()));
    file.close();

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(!database.needsUpgrade());
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::readError);
    }
    QCOMPARE(databaseHash(path), before);
}

void TestTrainDbVersionSafety::rebuildAllRollsBackOnFailure()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));
    QVERIFY(addNonUserSentinels(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("CREATE TRIGGER reject_rebuild_version "
                                       "BEFORE INSERT ON version "
                                       "WHEN NEW.table_name = 'workout_tag' "
                                       "BEGIN SELECT RAISE(ABORT, 'blocked'); END")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);
        QVERIFY(!database.rebuildDB());
        QVERIFY(database.schemaStatus()
                == TrainDB::SchemaStatus::writeError);
        QVERIFY(!database.deleteWorkout(QStringLiteral("/sentinel.erg")));
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::rebuildNonUserTablesRollsBackOnFailure()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QVERIFY(initializeCurrentDatabase(home.path()));
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(addCurrentSentinel(path));
    QVERIFY(addNonUserSentinels(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("CREATE TRIGGER reject_partial_rebuild_version "
                                       "BEFORE INSERT ON version "
                                       "WHEN NEW.table_name = 'videosync' "
                                       "BEGIN SELECT RAISE(ABORT, 'blocked'); END")));
    }

    const QByteArray before = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);
        QVERIFY(!database.rebuildDBButUserDataTables());
        QVERIFY(database.schemaStatus()
                == TrainDB::SchemaStatus::writeError);
        QVERIFY(!database.deleteWorkout(QStringLiteral("/sentinel.erg")));
    }
    QCOMPARE(databaseHash(path), before);
    QVERIFY(currentSentinelIsIntact(path));
}

void TestTrainDbVersionSafety::legacyDropRequiresVerifiedImport()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));

    TrainDB database{QDir(home.path())};
    QVERIFY(database.needsUpgrade());
    const QByteArray before = databaseHash(path);

    QVERIFY(!database.dropLegacyTables());
    QCOMPARE(databaseHash(path), before);
    QVERIFY(legacySentinelsAreIntact(path));
}

void TestTrainDbVersionSafety::dropLegacyTablesRollsBackOnFailure()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));

    TrainDB database{QDir(home.path())};
    QVERIFY(database.needsUpgrade());
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    LibraryImportResult result;
    QVERIFY(seedImportedRows(path, plan, result));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("CREATE TRIGGER reject_legacy_delete "
                                       "BEFORE DELETE ON version "
                                       "WHEN OLD.table_name = 'videosyncs' "
                                       "BEGIN SELECT RAISE(ABORT, 'blocked'); END")));
    }

    const QByteArray before = databaseHash(path);
    QVERIFY(!database.finalizeLegacyMigration(plan, result));
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
    QCOMPARE(databaseHash(path), before);
    QVERIFY(legacySentinelsAreIntact(path));
}

void TestTrainDbVersionSafety::finalizeRejectsChangedLegacyPlan()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));

    TrainDB database{QDir(home.path())};
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    LibraryImportResult result;
    QVERIFY(seedImportedRows(path, plan, result));

    QVERIFY(addSecondLegacyRows(path));
    const QByteArray before = databaseHash(path);
    QVERIFY(!database.finalizeLegacyMigration(plan, result));
    QCOMPARE(databaseHash(path), before);
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
    QVERIFY(legacySentinelsAreIntact(path));

    const TrainDB::LegacyMigrationPlan changed = database.legacyMigrationPlan();
    QVERIFY(changed.valid);
    QCOMPARE(changed.workouts.size(), 2);
    QCOMPARE(changed.videos.size(), 2);
    QCOMPARE(changed.videoSyncs.size(), 2);
}

void TestTrainDbVersionSafety::finalizeRequiresEveryImportedRow()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));
    QVERIFY(addSecondLegacyRows(path));

    TrainDB database{QDir(home.path())};
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    QCOMPARE(plan.workouts.size(), 2);
    QCOMPARE(plan.videos.size(), 2);
    QCOMPARE(plan.videoSyncs.size(), 2);

    LibraryImportResult complete;
    QVERIFY(seedImportedRows(path, plan, complete));
    const QByteArray before = databaseHash(path);

    LibraryImportResult missingWorkout = complete;
    missingWorkout.importedWorkouts.remove(plan.workouts.last());
    QVERIFY(!database.finalizeLegacyMigration(plan, missingWorkout));
    QCOMPARE(databaseHash(path), before);

    LibraryImportResult missingVideo = complete;
    missingVideo.importedVideos.remove(plan.videos.last());
    QVERIFY(!database.finalizeLegacyMigration(plan, missingVideo));
    QCOMPARE(databaseHash(path), before);

    LibraryImportResult missingVideoSync = complete;
    missingVideoSync.importedVideoSyncs.remove(plan.videoSyncs.last());
    QVERIFY(!database.finalizeLegacyMigration(plan, missingVideoSync));
    QCOMPARE(databaseHash(path), before);

    LibraryImportResult wrongTarget = complete;
    wrongTarget.importedWorkouts.insert(plan.workouts.first(),
                                        QStringLiteral("//1"));
    QVERIFY(wrongTarget.importedAll(
        plan.workouts, plan.videos, plan.videoSyncs));
    QVERIFY(!database.finalizeLegacyMigration(plan, wrongTarget));
    QCOMPARE(databaseHash(path), before);

    LibraryImportResult duplicateTarget = complete;
    duplicateTarget.importedWorkouts.insert(
        plan.workouts.last(),
        duplicateTarget.importedWorkouts.value(plan.workouts.first()));
    QVERIFY(duplicateTarget.importedAll(
        plan.workouts, plan.videos, plan.videoSyncs));
    QVERIFY(!database.finalizeLegacyMigration(plan, duplicateTarget));
    QCOMPARE(databaseHash(path), before);

    LibraryImportResult incompleteRequest = complete;
    QVERIFY(incompleteRequest.requestedFiles.removeOne(
        plan.files().first()));
    QVERIFY(!database.finalizeLegacyMigration(plan, incompleteRequest));
    QCOMPARE(databaseHash(path), before);

    QVERIFY(database.finalizeLegacyMigration(plan, complete));
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);
}

void TestTrainDbVersionSafety::finalizeLockedDatabaseIsUnchanged()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));

    TrainDB database{QDir(home.path())};
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    LibraryImportResult result;
    QVERIFY(seedImportedRows(path, plan, result));

    ScopedDatabase lock(path);
    QVERIFY(execSql(lock.get(), QStringLiteral("BEGIN EXCLUSIVE")));
    const QByteArray before = databaseHash(path);
    QVERIFY(!database.finalizeLegacyMigration(plan, result));
    QCOMPARE(databaseHash(path), before);
    QVERIFY(execSql(lock.get(), QStringLiteral("ROLLBACK")));

    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
    QVERIFY(legacySentinelsAreIntact(path));
}

void TestTrainDbVersionSafety::emptyLegacyMigrationFinalizes()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(), QStringLiteral("DELETE FROM workouts")));
        QVERIFY(execSql(connection.get(), QStringLiteral("DELETE FROM videos")));
        QVERIFY(execSql(connection.get(), QStringLiteral("DELETE FROM videosyncs")));
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("INSERT INTO workouts "
                                       "(filepath, filename) VALUES "
                                       "('//1', 'Manual Erg'), "
                                       "('//2', 'Manual Slope')")));
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("INSERT INTO videosyncs "
                                       "(filepath, filename) "
                                       "VALUES ('//1', 'None')")));
    }

    TrainDB database{QDir(home.path())};
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    QVERIFY(plan.isEmpty());

    LibraryImportResult noImportRequired;
    QVERIFY(database.finalizeLegacyMigration(plan, noImportRequired));
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);

    ScopedDatabase connection(path);
    QVERIFY(!tableExists(connection.get(), QStringLiteral("workouts")));
    QVERIFY(!tableExists(connection.get(), QStringLiteral("videos")));
    QVERIFY(!tableExists(connection.get(), QStringLiteral("videosyncs")));
}

void TestTrainDbVersionSafety::libraryResultGuardsLegacyDrop()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));

    TrainDB database{QDir(home.path())};
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    QVERIFY(!plan.workouts.isEmpty());
    QVERIFY(!plan.videoSyncs.isEmpty());

    LibraryImportResult complete;
    QVERIFY(seedImportedRows(path, plan, complete));
    const QString copiedWorkout = QStringLiteral("/library/legacy.erg");
    {
        ScopedDatabase connection(path);
        QSqlDatabase &sql = connection.get();
        QVERIFY(execSql(sql,
                        QStringLiteral("DELETE FROM workout "
                                       "WHERE filepath = '/legacy.erg'")));
        QVERIFY(execSql(sql,
                        QStringLiteral("INSERT INTO workout "
                                       "(filepath, type, displayname) VALUES "
                                       "('/library/legacy.erg', 'erg', 'Legacy')")));
    }
    complete.importedWorkouts.insert(plan.workouts.first(), copiedWorkout);

    LibraryImportResult incomplete = complete;
    const QString missing = plan.videoSyncs.first();
    incomplete.importedVideoSyncs.remove(missing);
    incomplete.failedFiles.append(missing);

    const QByteArray before = databaseHash(path);
    QVERIFY(!database.finalizeLegacyMigration(plan, incomplete));
    QCOMPARE(databaseHash(path), before);
    QVERIFY(legacySentinelsAreIntact(path));
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);

    QVERIFY(complete.allSucceeded());
    QVERIFY(database.finalizeLegacyMigration(plan, complete));
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);
    QVERIFY(!database.needsUpgrade());

    ScopedDatabase connection(path);
    QVERIFY(!tableExists(connection.get(), QStringLiteral("workouts")));
    QVERIFY(!tableExists(connection.get(), QStringLiteral("videos")));
    QVERIFY(!tableExists(connection.get(), QStringLiteral("videosyncs")));
    QSqlQuery migrated(connection.get());
    QVERIFY(migrated.exec(
        QStringLiteral("SELECT "
                       "(SELECT COUNT(*) FROM workout WHERE filepath = '/library/legacy.erg'), "
                       "(SELECT COUNT(*) FROM video WHERE filepath = '/legacy.mp4'), "
                       "(SELECT COUNT(*) FROM videosync WHERE filepath = '/legacy.rlv')")));
    QVERIFY(migrated.next());
    QCOMPARE(migrated.value(0).toInt(), 1);
    QCOMPARE(migrated.value(1).toInt(), 1);
    QCOMPARE(migrated.value(2).toInt(), 1);
}

void TestTrainDbVersionSafety::uncPathsAreMigrated()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));
    {
        ScopedDatabase connection(path);
        QSqlDatabase &sql = connection.get();
        QVERIFY(execSql(sql,
                        QStringLiteral("INSERT INTO workouts "
                                       "(filepath, filename) VALUES "
                                       "('//1', 'Manual Erg'), "
                                       "('//2', 'Manual Slope'), "
                                       "('//server/share/network.erg', 'Network')")));
        QVERIFY(execSql(sql,
                        QStringLiteral("INSERT INTO videosyncs "
                                       "(filepath, filename) VALUES "
                                       "('//1', 'None'), "
                                       "('//server/share/network.rlv', 'Network')")));
    }

    TrainDB database{QDir(home.path())};
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(plan.valid);
    QVERIFY(plan.workouts.contains(
        QStringLiteral("//server/share/network.erg")));
    QVERIFY(plan.videoSyncs.contains(
        QStringLiteral("//server/share/network.rlv")));
    QVERIFY(!plan.workouts.contains(QStringLiteral("//1")));
    QVERIFY(!plan.workouts.contains(QStringLiteral("//2")));
    QVERIFY(!plan.videoSyncs.contains(QStringLiteral("//1")));
}

void TestTrainDbVersionSafety::nullLegacyPathBlocksPlan()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));
    {
        ScopedDatabase connection(path);
        QVERIFY(execSql(connection.get(),
                        QStringLiteral("INSERT INTO workouts "
                                       "(filepath, filename) "
                                       "VALUES (NULL, 'Broken')")));
    }

    TrainDB database{QDir(home.path())};
    QVERIFY(database.needsUpgrade());
    const QByteArray before = databaseHash(path);
    const TrainDB::LegacyMigrationPlan plan = database.legacyMigrationPlan();
    QVERIFY(!plan.valid);
    QCOMPARE(databaseHash(path), before);

    LibraryImportResult result;
    QVERIFY(!database.finalizeLegacyMigration(plan, result));
    QCOMPARE(databaseHash(path), before);
    QVERIFY(legacySentinelsAreIntact(path));
}

void TestTrainDbVersionSafety::videoSyncImportSupportsMigrationRetry()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    TrainDB database{QDir(home.path())};
    QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::current);

    const QString path = QStringLiteral("/retry.rlv");
    VideoSyncFileBase initial;
    initial.name(QStringLiteral("Initial"));
    initial.source(QStringLiteral("legacy"));
    QVERIFY(database.importVideoSync(path, initial));

    VideoSyncFileBase updated;
    updated.name(QStringLiteral("Updated"));
    updated.source(QStringLiteral("migration-retry"));
    QVERIFY(database.importVideoSync(path, updated, ImportMode::insertOrUpdate));

    VideoSyncFileBase replaced;
    replaced.name(QStringLiteral("Replaced"));
    replaced.source(QStringLiteral("migration-replace"));
    QVERIFY(database.importVideoSync(path, replaced, ImportMode::replace));

    ScopedDatabase connection(home.path() + QStringLiteral("/trainDB"));
    QSqlQuery query(connection.get());
    query.prepare(QStringLiteral("SELECT source, displayname FROM videosync "
                                 "WHERE filepath = :filepath"));
    query.bindValue(QStringLiteral(":filepath"), path);
    QVERIFY(query.exec());
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QStringLiteral("migration-replace"));
    QCOMPARE(query.value(1).toString(), QStringLiteral("Replaced"));
    QVERIFY(!query.next());
}

void TestTrainDbVersionSafety::versionOneUpgradePreservesLegacyTables()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString path = home.path() + QStringLiteral("/trainDB");
    QVERIFY(createLegacyVersionOneDatabase(path));

    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.needsUpgrade());
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
        QVERIFY(database.getMigrateableWorkoutPaths().contains(QStringLiteral("/legacy.erg")));
        QVERIFY(database.getMigrateableVideoPaths().contains(QStringLiteral("/legacy.mp4")));
        QVERIFY(database.getMigrateableVideoSyncPaths().contains(QStringLiteral("/legacy.rlv")));
    }
    QVERIFY(legacySentinelsAreIntact(path));

    {
        ScopedDatabase connection(path);
        QSqlDatabase &database = connection.get();
        QSqlQuery currentVersions(database);
        QVERIFY(currentVersions.exec(
            QStringLiteral("SELECT COUNT(*) FROM version WHERE schema_version = 2")));
        QVERIFY(currentVersions.next());
        QCOMPARE(currentVersions.value(0).toInt(), 5);

        QSqlQuery legacyVersions(database);
        QVERIFY(legacyVersions.exec(
            QStringLiteral("SELECT COUNT(*) FROM version WHERE schema_version = 1")));
        QVERIFY(legacyVersions.next());
        QCOMPARE(legacyVersions.value(0).toInt(), 3);
    }

    const QByteArray completedUpgrade = databaseHash(path);
    {
        TrainDB database{QDir(home.path())};
        QVERIFY(database.schemaStatus() == TrainDB::SchemaStatus::migrationReady);
        QVERIFY(database.needsUpgrade());
    }
    QCOMPARE(databaseHash(path), completedUpgrade);
    QVERIFY(legacySentinelsAreIntact(path));
}

QTEST_GUILESS_MAIN(TestTrainDbVersionSafety)
#include "testTrainDbVersionSafety.moc"
