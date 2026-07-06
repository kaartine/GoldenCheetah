#include <QtTest>

#include "Athlete.h"
#include "Context.h"
#include "RideCache.h"
#include "RideItem.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <memory>

namespace {

QString firstName()
{
    return QStringLiteral("2026_07_06_08_00_00.json");
}

QString secondName()
{
    return QStringLiteral("2026_07_06_09_00_00.json");
}

void writeFixture(const QString &path, const QByteArray &contents)
{
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
};

void TestRideCacheRemoval::
archivedRemovalEvictsNamedRideWithoutMovingFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), true);

    const QByteArray decoy("new file at the old activity path");
    const QByteArray archived("already archived original");
    writeFixture(fixture.activityPath(firstName()), decoy);
    writeFixture(fixture.backupPath(firstName()), archived);
    writeFixture(
        fixture.cachePath(firstName(), QStringLiteral("notes")),
        QByteArray("derived notes"));

    QVERIFY(fixture.cache->removeArchivedRide(firstName()));

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

QTEST_MAIN(TestRideCacheRemoval)
#include "testRideCacheRemoval.moc"
