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
        athleteDirectory = temporary.filePath(QStringLiteral("athlete"));
        valid = QDir().mkpath(databaseHome)
            && QDir().mkpath(workoutDirectory)
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
    }

    ~TestEnvironment()
    {
        libraries.clear();
        trainDB = nullptr;
        database.reset();
        appsettings = nullptr;
    }

    QString filePath(const QString &name) const
    {
        return QDir(workoutDirectory).filePath(name);
    }

    QString databasePath() const
    {
        return QDir(databaseHome).filePath(QStringLiteral("trainDB"));
    }

    bool valid = false;
    QTemporaryDir temporary;
    QString databaseHome;
    QString workoutDirectory;
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

QTEST_GUILESS_MAIN(TestLibraryTransactionSafety)
#include "testLibraryTransactionSafety.moc"
