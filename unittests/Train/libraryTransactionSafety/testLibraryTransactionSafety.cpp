#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "LibraryTransactionTestStubs.h"
#include "TrainDB.h"
#include "WorkoutImportBatch.h"

namespace {

class ScopedDatabase
{
public:
    explicit ScopedDatabase(const QString &path)
        : name(QStringLiteral("library-transaction-reader-%1").arg(++nextId))
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

class TestEnvironment
{
public:
    TestEnvironment()
    {
        valid = temporary.isValid();
        if (!valid) return;

        databaseHome = temporary.filePath(QStringLiteral("database"));
        workoutDirectory = temporary.filePath(QStringLiteral("workouts"));
        sourceDirectory = temporary.filePath(QStringLiteral("sources"));
        athleteDirectory = temporary.filePath(QStringLiteral("athlete"));
        valid = QDir().mkpath(databaseHome)
            && QDir().mkpath(workoutDirectory)
            && QDir().mkpath(sourceDirectory)
            && QDir().mkpath(athleteDirectory);
        if (!valid) return;

        settings.workoutDirectory = workoutDirectory;
        appsettings = &settings;
        home = std::make_unique<TestAthleteHome>(QDir(athleteDirectory));
        athlete.home = home.get();
        context.athlete = &athlete;

        database = std::make_unique<TrainDB>(QDir(databaseHome));
        trainDB = database.get();

        mediaLibrary.name = QStringLiteral("Media Library");
        libraries.append(&mediaLibrary);
        LibraryParser::reset();
    }

    ~TestEnvironment()
    {
        libraries.clear();
        trainDB = nullptr;
        database.reset();
        appsettings = nullptr;
        LibraryParser::reset();
    }

    QString filePath(const QString &name) const
    {
        return QDir(workoutDirectory).filePath(name);
    }

    QString databasePath() const
    {
        return QDir(databaseHome).filePath(QStringLiteral("trainDB"));
    }

    QString sourcePath(const QString &name) const
    {
        return QDir(sourceDirectory).filePath(name);
    }

    QString libraryXmlPath() const
    {
        return temporary.filePath(QStringLiteral("library.xml"));
    }

    bool valid = false;
    QTemporaryDir temporary;
    QString databaseHome;
    QString workoutDirectory;
    QString sourceDirectory;
    QString athleteDirectory;
    TestAppSettings settings;
    std::unique_ptr<TestAthleteHome> home;
    Athlete athlete;
    Context context;
    Library mediaLibrary;
    std::unique_ptr<TrainDB> database;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size()
        && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool execSql(QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    if (query.exec(sql)) {
        return true;
    }
    qWarning().noquote() << query.lastError().text() << "for" << sql;
    return false;
}

QString sqlString(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'%1'").arg(escaped);
}

bool rowExists(QSqlDatabase &database,
               const QString &table,
               const QString &path)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM %1 WHERE filepath = :filepath")
                      .arg(table));
    query.bindValue(QStringLiteral(":filepath"), path);
    return query.exec() && query.next();
}

double storedAveragePower(QSqlDatabase &database,
                          const QString &path,
                          bool *found = nullptr)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT erg_avg_power FROM workout WHERE filepath = :filepath"));
    query.bindValue(QStringLiteral(":filepath"), path);
    const bool available = query.exec() && query.next();
    if (found != nullptr) {
        *found = available;
    }
    return available ? query.value(0).toDouble() : 0.0;
}

ErgFileBase workout(double averagePower, const QString &name)
{
    ErgFileBase file;
    file.format(ErgFileFormat::erg);
    file.name(name);
    file.AP(averagePower);
    return file;
}

bool installDeferredCommitFailure(QSqlDatabase &database,
                                  const QString &operation,
                                  const QString &path)
{
    return execSql(database, QStringLiteral("PRAGMA foreign_keys = ON"))
        && execSql(database,
                   QStringLiteral("CREATE TABLE deferred_parent "
                                  "(id INTEGER PRIMARY KEY)"))
        && execSql(database,
                   QStringLiteral("CREATE TABLE deferred_child ("
                                  "parent_id INTEGER, "
                                  "FOREIGN KEY(parent_id) "
                                  "REFERENCES deferred_parent(id) "
                                  "DEFERRABLE INITIALLY DEFERRED)"))
        && execSql(database,
                   QStringLiteral("CREATE TRIGGER deferred_commit_failure "
                                  "AFTER %1 ON workout "
                                  "WHEN NEW.filepath = %2 BEGIN "
                                  "INSERT INTO deferred_child(parent_id) "
                                  "VALUES (42); END")
                       .arg(operation, sqlString(path)));
}

} // namespace

class TestLibraryTransactionSafety : public QObject
{
    Q_OBJECT

private slots:
    void importFilesDuplicateFailureRollsBackBatch();
    void importFilesSchemaFailureRollsBackEarlierWrite();
    void importFilesCommitFailureRollsBackWithoutSignal();
    void importFilesSuccessSignalsAfterCommit();
    void refreshWorkoutsUpdateFailureDoesNotCommitPartialBatch();
    void refreshWorkoutsSchemaFailureDoesNotReportSuccess();
    void refreshWorkoutsCommitFailureRollsBackWithoutSignal();
    void refreshWorkoutsSuccessSignalsAfterCommit();
    void dialogBatchStartFailureDoesNotCommitOrAccept();
    void dialogBatchDuplicateFailureRollsBackEarlierWrite();
    void dialogBatchSchemaFailureRollsBackFilesAndDatabase();
    void dialogBatchImportFailureRollsBackEarlierWrite();
    void dialogBatchCommitFailureRestoresMetadataAndFiles();
    void dialogBatchTargetCollisionIsNotReportedAsSuccess();
    void dialogBatchOverwriteCopyFailurePreservesTarget();
    void dialogBatchLaterFailureRestoresOverwrittenTarget();
    void dialogBatchSerializeFailureRestoresAllStores();
    void dialogBatchSuccessPublishesOnlyAfterCommit();
};

void TestLibraryTransactionSafety::importFilesDuplicateFailureRollsBackBatch()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    QCOMPARE(environment.database->schemaStatus(),
             TrainDB::SchemaStatus::current);

    const QString first = environment.filePath(QStringLiteral("first.erg"));
    const QString duplicate = environment.filePath(
        QStringLiteral("duplicate.erg"));
    QVERIFY(writeFile(first, QByteArrayLiteral("210")));
    QVERIFY(writeFile(duplicate, QByteArrayLiteral("260")));
    QVERIFY(environment.database->importWorkout(
        duplicate, workout(125.0, QStringLiteral("Original"))));

    QSqlDatabase connection = QSqlDatabase::database(
        QStringLiteral("train"), false);
    QVERIFY(execSql(
        connection,
        QStringLiteral("CREATE TRIGGER reject_duplicate_update "
                       "BEFORE UPDATE ON workout "
                       "WHEN OLD.filepath = %1 BEGIN "
                       "SELECT RAISE(FAIL, 'duplicate rejected'); END")
            .arg(sqlString(duplicate))));

    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    const LibraryImportResult result = Library::importFiles(
        &environment.context,
        {first, duplicate},
        LibraryBatchImportConfirmation::noDialog);

    QVERIFY(result.completed);
    QVERIFY(!result.allSucceeded());
    QVERIFY(result.failedFiles.contains(first));
    QVERIFY(result.failedFiles.contains(duplicate));
    QVERIFY(!environment.database->hasWorkout(first));
    QCOMPARE(storedAveragePower(connection, duplicate), 125.0);
    QCOMPARE(changed.count(), 0);
}

void TestLibraryTransactionSafety::importFilesSchemaFailureRollsBackEarlierWrite()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    const QString video = environment.filePath(QStringLiteral("first.mp4"));
    const QString workoutPath = environment.filePath(
        QStringLiteral("missing-schema.erg"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(workoutPath, QByteArrayLiteral("230")));

    QSqlDatabase connection = QSqlDatabase::database(
        QStringLiteral("train"), false);
    QVERIFY(execSql(connection, QStringLiteral("DROP TABLE workout")));

    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    const LibraryImportResult result = Library::importFiles(
        &environment.context,
        {video, workoutPath},
        LibraryBatchImportConfirmation::noDialog);

    QVERIFY(result.completed);
    QVERIFY(!result.allSucceeded());
    QVERIFY(!environment.database->hasVideo(video));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QCOMPARE(changed.count(), 0);
}

void TestLibraryTransactionSafety::importFilesCommitFailureRollsBackWithoutSignal()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    const QString path = environment.filePath(
        QStringLiteral("commit-failure.erg"));
    QVERIFY(writeFile(path, QByteArrayLiteral("205")));
    QSqlDatabase connection = QSqlDatabase::database(
        QStringLiteral("train"), false);
    QVERIFY(installDeferredCommitFailure(
        connection, QStringLiteral("INSERT"), path));

    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    const LibraryImportResult result = Library::importFiles(
        &environment.context,
        {path},
        LibraryBatchImportConfirmation::noDialog);

    QVERIFY(result.completed);
    QVERIFY(!result.allSucceeded());
    QVERIFY(!environment.database->hasWorkout(path));
    QCOMPARE(changed.count(), 0);
}

void TestLibraryTransactionSafety::importFilesSuccessSignalsAfterCommit()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    const QString path = environment.filePath(QStringLiteral("success.erg"));
    QVERIFY(writeFile(path, QByteArrayLiteral("245")));
    ScopedDatabase reader(environment.databasePath());
    QVERIFY(reader.get().isOpen());

    bool committedRowObserved = false;
    connect(environment.database.get(), &TrainDB::dataChanged, this, [&] {
        committedRowObserved = rowExists(
            reader.get(), QStringLiteral("workout"), path);
    });
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);

    const LibraryImportResult result = Library::importFiles(
        &environment.context,
        {path},
        LibraryBatchImportConfirmation::noDialog);

    QVERIFY(result.allSucceeded());
    QCOMPARE(changed.count(), 1);
    QVERIFY(committedRowObserved);
    QCOMPARE(environment.context.selectedWorkout, path);
}

void TestLibraryTransactionSafety::
refreshWorkoutsUpdateFailureDoesNotCommitPartialBatch()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    const QString first = environment.filePath(
        QStringLiteral("refresh-first.erg"));
    const QString second = environment.filePath(
        QStringLiteral("refresh-second.erg"));
    QVERIFY(writeFile(first, QByteArrayLiteral("220")));
    QVERIFY(writeFile(second, QByteArrayLiteral("280")));
    QVERIFY(environment.database->importWorkout(
        first, workout(110.0, QStringLiteral("First"))));
    QVERIFY(environment.database->importWorkout(
        second, workout(140.0, QStringLiteral("Second"))));

    QSqlDatabase connection = QSqlDatabase::database(
        QStringLiteral("train"), false);
    QVERIFY(execSql(connection,
                    QStringLiteral("CREATE TABLE refresh_update_counter "
                                   "(value INTEGER NOT NULL)")));
    QVERIFY(execSql(connection,
                    QStringLiteral("INSERT INTO refresh_update_counter "
                                   "VALUES (0)")));
    QVERIFY(execSql(connection,
                    QStringLiteral("CREATE TRIGGER reject_second_refresh "
                                   "BEFORE UPDATE ON workout "
                                   "WHEN OLD.filepath NOT LIKE '//%' BEGIN "
                                   "UPDATE refresh_update_counter "
                                   "SET value = value + 1; "
                                   "SELECT CASE WHEN "
                                   "(SELECT value FROM refresh_update_counter) = 2 "
                                   "THEN RAISE(FAIL, 'second update rejected') END; "
                                   "END")));

    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    QVERIFY(!Library::refreshWorkouts(&environment.context));

    QCOMPARE(storedAveragePower(connection, first), 110.0);
    QCOMPARE(storedAveragePower(connection, second), 140.0);
    QCOMPARE(changed.count(), 0);
}

void TestLibraryTransactionSafety::
refreshWorkoutsSchemaFailureDoesNotReportSuccess()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    QSqlDatabase connection = QSqlDatabase::database(
        QStringLiteral("train"), false);
    QVERIFY(execSql(connection, QStringLiteral("DROP TABLE workout")));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);

    QVERIFY(!Library::refreshWorkouts(&environment.context));
    QCOMPARE(changed.count(), 0);
}

void TestLibraryTransactionSafety::
refreshWorkoutsCommitFailureRollsBackWithoutSignal()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    const QString path = environment.filePath(
        QStringLiteral("refresh-commit-failure.erg"));
    QVERIFY(writeFile(path, QByteArrayLiteral("275")));
    QVERIFY(environment.database->importWorkout(
        path, workout(135.0, QStringLiteral("Original"))));

    QSqlDatabase connection = QSqlDatabase::database(
        QStringLiteral("train"), false);
    QVERIFY(installDeferredCommitFailure(
        connection, QStringLiteral("UPDATE"), path));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);

    QVERIFY(!Library::refreshWorkouts(&environment.context));
    QCOMPARE(storedAveragePower(connection, path), 135.0);
    QCOMPARE(changed.count(), 0);
}

void TestLibraryTransactionSafety::refreshWorkoutsSuccessSignalsAfterCommit()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);

    const QString path = environment.filePath(
        QStringLiteral("refresh-success.erg"));
    QVERIFY(writeFile(path, QByteArrayLiteral("265")));
    QVERIFY(environment.database->importWorkout(
        path, workout(130.0, QStringLiteral("Original"))));
    ScopedDatabase reader(environment.databasePath());
    QVERIFY(reader.get().isOpen());

    bool committedValueObserved = false;
    connect(environment.database.get(), &TrainDB::dataChanged, this, [&] {
        bool found = false;
        const double value = storedAveragePower(reader.get(), path, &found);
        committedValueObserved = found && qFuzzyCompare(value, 265.0);
    });
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);

    QVERIFY(Library::refreshWorkouts(&environment.context));
    QCOMPARE(changed.count(), 1);
    QVERIFY(committedValueObserved);
}

void TestLibraryTransactionSafety::dialogBatchStartFailureDoesNotCommitOrAccept()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString video = environment.sourcePath(QStringLiteral("nested.mp4"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(environment.libraryXmlPath(), QByteArrayLiteral("original")));

    TrainDB::ScopedLUW outer(*environment.database);
    QVERIFY(outer.isActive());
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;
    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context, {video}, {}, {}, false, [&] { ++accepted; });
    outer.rollback();

    QVERIFY(!result.succeeded);
    QCOMPARE(accepted, 0);
    QCOMPARE(changed.count(), 0);
    QVERIFY(!environment.database->hasVideo(video));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QCOMPARE(readFile(environment.libraryXmlPath()), QByteArrayLiteral("original"));
}

void TestLibraryTransactionSafety::dialogBatchDuplicateFailureRollsBackEarlierWrite()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString first = environment.sourcePath(QStringLiteral("first.mp4"));
    const QString duplicate =
        environment.sourcePath(QStringLiteral("duplicate.mp4"));
    QVERIFY(writeFile(first, QByteArrayLiteral("first")));
    QVERIFY(writeFile(duplicate, QByteArrayLiteral("duplicate")));
    QVERIFY(environment.database->importVideo(duplicate));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {first, duplicate},
        {},
        {},
        false,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QVERIFY(result.failedFiles.contains(first));
    QVERIFY(result.failedFiles.contains(duplicate));
    QVERIFY(!environment.database->hasVideo(first));
    QVERIFY(environment.database->hasVideo(duplicate));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchSchemaFailureRollsBackFilesAndDatabase()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString video = environment.sourcePath(QStringLiteral("before.mp4"));
    const QString source =
        environment.sourcePath(QStringLiteral("schema-failure.erg"));
    const QString target = environment.filePath(QStringLiteral("schema-failure.erg"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(source, QByteArrayLiteral("225")));
    QVERIFY(writeFile(environment.libraryXmlPath(), QByteArrayLiteral("original")));
    QSqlDatabase connection = QSqlDatabase::database(QStringLiteral("train"), false);
    QVERIFY(execSql(connection, QStringLiteral("DROP TABLE workout")));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {video},
        {source},
        {},
        false,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QVERIFY(!environment.database->hasVideo(video));
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QCOMPARE(LibraryParser::serializeCalls, 0);
    QCOMPARE(readFile(environment.libraryXmlPath()), QByteArrayLiteral("original"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchImportFailureRollsBackEarlierWrite()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString video = environment.sourcePath(QStringLiteral("first.mp4"));
    const QString source =
        environment.sourcePath(QStringLiteral("rejected.erg"));
    const QString target = environment.filePath(QStringLiteral("rejected.erg"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(source, QByteArrayLiteral("235")));
    QSqlDatabase connection = QSqlDatabase::database(QStringLiteral("train"), false);
    QVERIFY(execSql(connection,
                    QStringLiteral("CREATE TRIGGER reject_dialog_workout "
                                   "BEFORE INSERT ON workout BEGIN "
                                   "SELECT RAISE(FAIL, 'workout rejected'); END")));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {video},
        {source},
        {},
        false,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QVERIFY(!environment.database->hasVideo(video));
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchCommitFailureRestoresMetadataAndFiles()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString video = environment.sourcePath(QStringLiteral("commit.mp4"));
    const QString source =
        environment.sourcePath(QStringLiteral("commit-failure.erg"));
    const QString target =
        environment.filePath(QStringLiteral("commit-failure.erg"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(source, QByteArrayLiteral("245")));
    QVERIFY(writeFile(environment.libraryXmlPath(), QByteArrayLiteral("original")));
    QSqlDatabase connection = QSqlDatabase::database(QStringLiteral("train"), false);
    QVERIFY(installDeferredCommitFailure(
        connection, QStringLiteral("INSERT"), target));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {video},
        {source},
        {},
        false,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QVERIFY(!environment.database->hasVideo(video));
    QVERIFY(!environment.database->hasWorkout(target));
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QCOMPARE(readFile(environment.libraryXmlPath()), QByteArrayLiteral("original"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchTargetCollisionIsNotReportedAsSuccess()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString source = environment.sourcePath(QStringLiteral("collision.erg"));
    const QString target = environment.filePath(QStringLiteral("collision.erg"));
    QVERIFY(writeFile(source, QByteArrayLiteral("255")));
    QVERIFY(writeFile(target, QByteArrayLiteral("old target")));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {},
        {source},
        {},
        false,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QCOMPARE(readFile(target), QByteArrayLiteral("old target"));
    QVERIFY(!environment.database->hasWorkout(target));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchOverwriteCopyFailurePreservesTarget()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString source = environment.sourcePath(QStringLiteral("overwrite.erg"));
    const QString target = environment.filePath(QStringLiteral("overwrite.erg"));
    QVERIFY(writeFile(source, QByteArrayLiteral("265")));
    QVERIFY(writeFile(target, QByteArrayLiteral("old target")));
    const QFileDevice::Permissions originalPermissions =
        QFileInfo(environment.workoutDirectory).permissions();
    QVERIFY(QFile::setPermissions(
        environment.workoutDirectory,
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {},
        {source},
        {},
        true,
        [&] { ++accepted; });
    QVERIFY(QFile::setPermissions(environment.workoutDirectory,
                                  originalPermissions));

    QVERIFY(!result.succeeded);
    QCOMPARE(readFile(target), QByteArrayLiteral("old target"));
    QVERIFY(!environment.database->hasWorkout(target));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchLaterFailureRestoresOverwrittenTarget()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString firstSource =
        environment.sourcePath(QStringLiteral("replace-first.erg"));
    const QString firstTarget =
        environment.filePath(QStringLiteral("replace-first.erg"));
    const QString secondSource =
        environment.sourcePath(QStringLiteral("replace-second.erg"));
    const QString secondTarget =
        environment.filePath(QStringLiteral("replace-second.erg"));
    QVERIFY(writeFile(firstSource, QByteArrayLiteral("300")));
    QVERIFY(writeFile(firstTarget, QByteArrayLiteral("100")));
    QVERIFY(writeFile(secondSource, QByteArrayLiteral("325")));
    QVERIFY(environment.database->importWorkout(
        firstTarget, workout(100.0, QStringLiteral("Original"))));
    QSqlDatabase connection = QSqlDatabase::database(QStringLiteral("train"), false);
    QVERIFY(execSql(
        connection,
        QStringLiteral("CREATE TRIGGER reject_later_workout "
                       "BEFORE INSERT ON workout "
                       "WHEN NEW.filepath = %1 BEGIN "
                       "SELECT RAISE(FAIL, 'later workout rejected'); END")
            .arg(sqlString(secondTarget))));
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {},
        {firstSource, secondSource},
        {},
        true,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QCOMPARE(readFile(firstTarget), QByteArrayLiteral("100"));
    QVERIFY(!QFileInfo::exists(secondTarget));
    QCOMPARE(storedAveragePower(connection, firstTarget), 100.0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchSerializeFailureRestoresAllStores()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString video = environment.sourcePath(QStringLiteral("serialize.mp4"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(environment.libraryXmlPath(), QByteArrayLiteral("original")));
    LibraryParser::serializeResult = false;
    LibraryParser::writeBeforeReturning = true;
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);
    int accepted = 0;

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {video},
        {},
        {},
        false,
        [&] { ++accepted; });

    QVERIFY(!result.succeeded);
    QVERIFY(!environment.database->hasVideo(video));
    QVERIFY(environment.mediaLibrary.refs.isEmpty());
    QVERIFY(environment.context.selectedVideo.isEmpty());
    QCOMPARE(readFile(environment.libraryXmlPath()), QByteArrayLiteral("original"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(accepted, 0);
}

void TestLibraryTransactionSafety::dialogBatchSuccessPublishesOnlyAfterCommit()
{
    TestEnvironment environment;
    QVERIFY(environment.valid);
    const QString video = environment.sourcePath(QStringLiteral("success.mp4"));
    const QString workoutSource =
        environment.sourcePath(QStringLiteral("success.erg"));
    const QString workoutTarget = environment.filePath(QStringLiteral("success.erg"));
    const QString videoSyncSource =
        environment.sourcePath(QStringLiteral("success.rlv"));
    const QString videoSyncTarget = environment.filePath(QStringLiteral("success.rlv"));
    QVERIFY(writeFile(video, QByteArrayLiteral("video")));
    QVERIFY(writeFile(workoutSource, QByteArrayLiteral("275")));
    QVERIFY(writeFile(videoSyncSource, QByteArrayLiteral("videosync")));
    QVERIFY(writeFile(environment.libraryXmlPath(), QByteArrayLiteral("original")));
    ScopedDatabase reader(environment.databasePath());
    QVERIFY(reader.get().isOpen());

    int accepted = 0;
    bool committedRowsObserved = false;
    bool publicationDeferredUntilAfterCommit = false;
    bool acceptObservedPublishedState = false;
    connect(environment.database.get(), &TrainDB::dataChanged, this, [&] {
        committedRowsObserved =
            rowExists(reader.get(), QStringLiteral("video"), video)
            && rowExists(reader.get(), QStringLiteral("workout"), workoutTarget)
            && rowExists(reader.get(), QStringLiteral("videosync"), videoSyncTarget);
        publicationDeferredUntilAfterCommit =
            environment.mediaLibrary.refs.isEmpty()
            && environment.context.selectedVideo.isEmpty()
            && environment.context.selectedWorkout.isEmpty()
            && environment.context.selectedVideoSync.isEmpty()
            && accepted == 0;
    });
    QSignalSpy changed(environment.database.get(), &TrainDB::dataChanged);

    const WorkoutImportBatchResult result = runWorkoutImportDialogBatch(
        &environment.context,
        {video},
        {workoutSource},
        {videoSyncSource},
        false,
        [&] {
            ++accepted;
            acceptObservedPublishedState =
                environment.mediaLibrary.refs.contains(video)
                && environment.context.selectedVideo == video
                && environment.context.selectedWorkout == workoutTarget
                && environment.context.selectedVideoSync == videoSyncTarget;
        });

    QVERIFY(result.succeeded);
    QCOMPARE(changed.count(), 1);
    QVERIFY(committedRowsObserved);
    QVERIFY(publicationDeferredUntilAfterCommit);
    QCOMPARE(accepted, 1);
    QVERIFY(acceptObservedPublishedState);
    QVERIFY(readFile(environment.libraryXmlPath()).contains(video.toUtf8()));
}

QTEST_GUILESS_MAIN(TestLibraryTransactionSafety)
#include "testLibraryTransactionSafety.moc"
