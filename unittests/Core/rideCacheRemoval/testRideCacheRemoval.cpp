#include <QtTest>

#include "Athlete.h"
#include "Context.h"
#include "RideCache.h"
#include "RideItem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <memory>

void resetRideCacheRemovalRefreshCounts();
int rideCacheRemovalRefreshCount();
int rideCacheRemovalEstimatorRefreshCount();

namespace {

QString firstName()
{
    return QStringLiteral("2026_07_06_08_00_00.json");
}

QString secondName()
{
    return QStringLiteral("2026_07_06_09_00_00.json");
}

QString thirdName()
{
    return QStringLiteral("2026_07_06_10_00_00.json");
}

void writeFixture(const QString &path, const QByteArray &contents)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY2(
        file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
    QVERIFY(file.flush());
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool cacheContains(const RideCache &cache, const QString &fileName)
{
    for (const RideItem *item :
         const_cast<RideCache &>(cache).rides()) {
        if (item && item->fileName == fileName) return true;
    }
    return false;
}

struct Fixture
{
    bool initialize()
    {
        if (!temporary.isValid()) return false;
        context.reset(new Context(nullptr));
        athlete.reset(new Athlete(context.get(), QDir(temporary.path())));
        cache.reset(new RideCache(context.get()));
        athlete->rideCache = cache.get();
        resetRideCacheRemovalRefreshCounts();
        return athlete->home->activities().exists()
            && athlete->home->fileBackup().exists()
            && athlete->home->cache().exists();
    }

    RideItem *addRide(const QString &fileName, bool current)
    {
        RideItem *item = new RideItem(nullptr, context.get());
        item->fileName = fileName;
        cache->rides().append(item);
        if (current) context->ride = item;
        return item;
    }

    QString activityPath(const QString &fileName) const
    {
        return athlete->home->activities().filePath(fileName);
    }

    QString plannedActivityPath(const QString &fileName) const
    {
        return athlete->home->planned().filePath(fileName);
    }

    QString backupPath(const QString &fileName) const
    {
        return athlete->home->fileBackup().filePath(
            fileName + QStringLiteral(".bak"));
    }

    QString cachePath(const QString &fileName, const QString &extension) const
    {
        return athlete->home->cache().filePath(
            QFileInfo(fileName).baseName()
            + QLatin1Char('.') + extension);
    }

    QString plannedCachePath(
        const QString &fileName,
        const QString &extension) const
    {
        return QDir(
            athlete->home->cache().filePath(
                QStringLiteral("planned")))
            .filePath(
                QFileInfo(fileName).baseName()
                + QLatin1Char('.') + extension);
    }

    QTemporaryDir temporary;
    std::unique_ptr<Context> context;
    std::unique_ptr<Athlete> athlete;
    std::unique_ptr<RideCache> cache;
};

} // namespace

class TestRideCacheRemoval : public QObject
{
    Q_OBJECT

private slots:
    void archivedRemovalEvictsNamedRideWithoutMovingFiles();
    void ordinaryRemovalArchivesFileAndReplacesBackup();
    void currentRideRemovalUsesOrdinaryArchivePath();
    void missingRideIsRejectedWithoutTouchingFiles();
    void plannedRemovalDeletesOnlyPlannedCpx();
    void plannedBatchRemovalDeletesOnlyPlannedCpx();
    void batchRemovalRefreshesOnce();
    void batchRemovalSnapshotsAliasedRideList();
    void currentRemovalUsesSelectedNamespace();
    void ambiguousFilenameRemovalFailsClosed();
    void explicitNamespaceRemovalUsesIdentity();
    void explicitBatchRemovalUsesItemIdentity();
    void plannedRenameMovesOnlyPlannedCpx();
    void completedRenameMovesOnlyCompletedCpx();
};

void TestRideCacheRemoval::
archivedRemovalEvictsNamedRideWithoutMovingFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *archivedItem =
        fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), true);

    const QByteArray decoy("new file at the old activity path");
    const QByteArray archived("already archived original");
    writeFixture(fixture.activityPath(firstName()), decoy);
    writeFixture(fixture.backupPath(firstName()), archived);
    writeFixture(
        fixture.cachePath(firstName(), QStringLiteral("notes")),
        QByteArray("derived notes"));

    QVERIFY(fixture.cache->removeArchivedRide(archivedItem));

    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(!cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QCOMPARE(fixture.context->ride->fileName, secondName());
    QCOMPARE(readBytes(fixture.activityPath(firstName())), decoy);
    QCOMPARE(readBytes(fixture.backupPath(firstName())), archived);
    QVERIFY(!QFileInfo::exists(
        fixture.cachePath(firstName(), QStringLiteral("notes"))));
}

void TestRideCacheRemoval::ordinaryRemovalArchivesFileAndReplacesBackup()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);

    const QByteArray original("live original");
    writeFixture(fixture.activityPath(firstName()), original);
    writeFixture(
        fixture.backupPath(firstName()),
        QByteArray("previous backup"));

    QVERIFY(fixture.cache->removeRide(firstName()));

    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(fixture.activityPath(firstName())));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), original);
}

void TestRideCacheRemoval::currentRideRemovalUsesOrdinaryArchivePath()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), true);

    const QByteArray original("current original");
    writeFixture(fixture.activityPath(firstName()), original);

    QVERIFY(fixture.cache->removeCurrentRide());

    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(fixture.activityPath(firstName())));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), original);
}

void TestRideCacheRemoval::missingRideIsRejectedWithoutTouchingFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(secondName(), true);

    const QByteArray contents("unrelated activity");
    writeFixture(fixture.activityPath(secondName()), contents);

    QVERIFY(!fixture.cache->removeArchivedRide(firstName()));

    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QCOMPARE(readBytes(fixture.activityPath(secondName())), contents);
}

void TestRideCacheRemoval::plannedRemovalDeletesOnlyPlannedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *planned = fixture.addRide(firstName(), false);
    planned->planned = true;
    fixture.addRide(secondName(), true);

    const QString completedCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QByteArray completedContents(
        "completed cache with matching basename");
    writeFixture(completedCpx, completedContents);
    writeFixture(
        plannedCpx,
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeArchivedRide(firstName()));

    QCOMPARE(readBytes(completedCpx), completedContents);
    QVERIFY(!QFileInfo::exists(plannedCpx));
}

void TestRideCacheRemoval::
plannedBatchRemovalDeletesOnlyPlannedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *planned =
        fixture.addRide(firstName(), false);
    planned->planned = true;
    fixture.addRide(secondName(), true);

    const QString plannedActivity =
        fixture.plannedActivityPath(
            firstName());
    const QString completedActivity =
        fixture.activityPath(
            secondName());
    const QString completedDecoyCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedTargetCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString completedTargetCpx =
        fixture.cachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString plannedDecoyCpx =
        fixture.plannedCachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QByteArray completedDecoyContents(
        "completed cache with matching basename");
    const QByteArray plannedDecoyContents(
        "planned cache with matching basename");
    writeFixture(
        plannedActivity,
        QByteArray("planned activity"));
    writeFixture(
        completedActivity,
        QByteArray("completed activity"));
    writeFixture(
        completedDecoyCpx,
        completedDecoyContents);
    writeFixture(
        plannedTargetCpx,
        QByteArray("planned target cache"));
    writeFixture(
        completedTargetCpx,
        QByteArray("completed target cache"));
    writeFixture(
        plannedDecoyCpx,
        plannedDecoyContents);

    QVERIFY(fixture.cache->removeRides(
        {firstName(), secondName()}, false));

    QVERIFY(!QFileInfo::exists(
        plannedActivity));
    QVERIFY(!QFileInfo::exists(
        completedActivity));
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(
        readBytes(completedDecoyCpx),
        completedDecoyContents);
    QVERIFY(!QFileInfo::exists(
        plannedTargetCpx));
    QVERIFY(!QFileInfo::exists(
        completedTargetCpx));
    QCOMPARE(
        readBytes(plannedDecoyCpx),
        plannedDecoyContents);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        0);
}

void TestRideCacheRemoval::batchRemovalRefreshesOnce()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), true);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("first activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("second activity"));
    QVERIFY(fixture.cache->removeRides(
        {firstName(), secondName()}));

    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        1);
}

void TestRideCacheRemoval::
batchRemovalSnapshotsAliasedRideList()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), false);
    fixture.addRide(thirdName(), true);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("first activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("second activity"));
    writeFixture(
        fixture.activityPath(thirdName()),
        QByteArray("third activity"));

    QVERIFY(fixture.cache->removeRides(
        fixture.cache->rides(), false));

    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(secondName())));
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(thirdName())));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        0);
}

void TestRideCacheRemoval::
currentRemovalUsesSelectedNamespace()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), false);
    RideItem *planned =
        fixture.addRide(firstName(), true);
    planned->planned = true;

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray plannedActivity(
        "planned activity");
    const QByteArray completedCache(
        "completed cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        plannedActivity);
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeCurrentRide());

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(fixture.context->ride, completed);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(
            firstName())));
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        plannedActivity);
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"))));
}

void TestRideCacheRemoval::
ambiguousFilenameRemovalFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), true);
    RideItem *planned =
        fixture.addRide(firstName(), false);
    planned->planned = true;
    fixture.context->ride = nullptr;

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray plannedActivity(
        "planned activity");
    const QByteArray completedCache(
        "completed cache");
    const QByteArray plannedCache(
        "planned cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        plannedActivity);
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        plannedCache);

    QVERIFY(!fixture.cache->removeRide(firstName()));
    QVERIFY(!fixture.cache->removeRides(
        {firstName()}, false));

    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(fixture.cache->rides().at(0), completed);
    QCOMPARE(fixture.cache->rides().at(1), planned);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QCOMPARE(
        readBytes(
            fixture.plannedActivityPath(
                firstName())),
        plannedActivity);
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QCOMPARE(
        readBytes(
            fixture.plannedCachePath(
                firstName(),
                QStringLiteral("cpx"))),
        plannedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
explicitNamespaceRemovalUsesIdentity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), true);
    RideItem *planned =
        fixture.addRide(firstName(), false);
    planned->planned = true;

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray completedCache(
        "completed cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned activity"));
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeRide(
        firstName(), true));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(
            firstName())));
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"))));
}

void TestRideCacheRemoval::
explicitBatchRemovalUsesItemIdentity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), true);
    RideItem *planned =
        fixture.addRide(firstName(), false);
    planned->planned = true;

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray completedCache(
        "completed cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned activity"));
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeRides(
        QList<RideItem*>{planned}, false));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(
            firstName())));
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"))));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        0);
}

void TestRideCacheRemoval::plannedRenameMovesOnlyPlannedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString oldActivity =
        fixture.plannedActivityPath(firstName());
    const QString newActivity =
        fixture.plannedActivityPath(secondName());
    const QString completedOldCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString completedNewCpx =
        fixture.cachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString plannedOldCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedNewCpx =
        fixture.plannedCachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString oldNotes =
        fixture.cachePath(
            firstName(),
            QStringLiteral("notes"));
    const QString newNotes =
        fixture.cachePath(
            secondName(),
            QStringLiteral("notes"));
    const QByteArray activityContents(
        "planned activity");
    const QByteArray completedContents(
        "completed cache with matching basename");
    const QByteArray plannedContents(
        "planned cache");
    const QByteArray notesContents(
        "shared notes");
    writeFixture(
        oldActivity, activityContents);
    writeFixture(
        completedOldCpx,
        completedContents);
    writeFixture(
        plannedOldCpx,
        plannedContents);
    writeFixture(
        oldNotes, notesContents);

    QString error;
    QVERIFY2(
        fixture.cache->renameRideFilesForTest(
            firstName(),
            secondName(),
            true,
            error),
        qPrintable(error));

    QVERIFY(!QFileInfo::exists(oldActivity));
    QCOMPARE(
        readBytes(newActivity),
        activityContents);
    QCOMPARE(
        readBytes(completedOldCpx),
        completedContents);
    QVERIFY(!QFileInfo::exists(
        completedNewCpx));
    QVERIFY(!QFileInfo::exists(
        plannedOldCpx));
    QCOMPARE(
        readBytes(plannedNewCpx),
        plannedContents);
    QVERIFY(!QFileInfo::exists(oldNotes));
    QCOMPARE(
        readBytes(newNotes),
        notesContents);
}

void TestRideCacheRemoval::
completedRenameMovesOnlyCompletedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString oldActivity =
        fixture.activityPath(firstName());
    const QString newActivity =
        fixture.activityPath(secondName());
    const QString completedOldCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString completedNewCpx =
        fixture.cachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString plannedOldCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedNewCpx =
        fixture.plannedCachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QByteArray activityContents(
        "completed activity");
    const QByteArray completedContents(
        "completed cache");
    const QByteArray plannedContents(
        "planned cache with matching basename");
    writeFixture(
        oldActivity, activityContents);
    writeFixture(
        completedOldCpx,
        completedContents);
    writeFixture(
        plannedOldCpx,
        plannedContents);

    QString error;
    QVERIFY2(
        fixture.cache->renameRideFilesForTest(
            firstName(),
            secondName(),
            false,
            error),
        qPrintable(error));

    QVERIFY(!QFileInfo::exists(oldActivity));
    QCOMPARE(
        readBytes(newActivity),
        activityContents);
    QVERIFY(!QFileInfo::exists(
        completedOldCpx));
    QCOMPARE(
        readBytes(completedNewCpx),
        completedContents);
    QCOMPARE(
        readBytes(plannedOldCpx),
        plannedContents);
    QVERIFY(!QFileInfo::exists(
        plannedNewCpx));
}

QTEST_MAIN(TestRideCacheRemoval)
#include "testRideCacheRemoval.moc"
