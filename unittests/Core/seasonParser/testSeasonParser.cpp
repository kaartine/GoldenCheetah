#include "Core/Seasons.h"
#include "Core/Season.h"
#include "FileIO/AtomicFileWriter.h"

#include <QTest>
#include <QFile>
#include <QList>
#include <QTemporaryDir>

#include <memory>


namespace {

enum class FailurePoint
{
    Open,
    Write,
    Flush,
    Commit
};

class FaultInjectingWriter final : public AtomicFileWriter
{
public:
    explicit FaultInjectingWriter(FailurePoint failure)
        : failure_(failure)
    {
    }

    bool open() override
    {
        return failure_ != FailurePoint::Open;
    }

    qint64 write(const QByteArray &data) override
    {
        return failure_ == FailurePoint::Write
            ? data.size() - 1
            : data.size();
    }

    bool flush() override
    {
        return failure_ != FailurePoint::Flush;
    }

    bool commit() override
    {
        return failure_ != FailurePoint::Commit;
    }

    void cancelWriting() override {}

    QString errorString() const override
    {
        return QStringLiteral("injected persistent write failure");
    }

private:
    FailurePoint failure_;
};

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(data) == data.size();
}

QList<Season> oneSeason()
{
    Season season;
    season.setName(QStringLiteral("Atomic & <season>"));
    season.setAbsoluteStart(QDate(2026, 1, 1));
    season.setAbsoluteEnd(QDate(2026, 12, 31));
    season.setType(Season::season);
    season.setId(QUuid(QStringLiteral(
        "{762dd017-6449-48bd-aa8d-25fe035dfbda}")));
    return QList<Season>({season});
}

} // namespace


class TestSeasonParser: public QObject
{
    Q_OBJECT

private slots:
    void failedWritesPreservePreviousFile_data();
    void failedWritesPreservePreviousFile();
    void successfulWritePublishesCompleteXml();

    void readSeasons() {
        QFile file("seasons.xml");
        QList<Season> seasons = SeasonParser::readSeasons(&file);

        QCOMPARE(seasons.size(), 5);

        int sx = 0;

        QCOMPARE(seasons[sx].getName(), "3-0 months ago");
        QCOMPARE(seasons[sx].getType(), Season::season);
        QCOMPARE(seasons[sx].id().toString(), "{728c23b7-d576-482c-a16a-d28c2ea9d76f}");
        QCOMPARE(seasons[sx].getSeed(), 0);
        QCOMPARE(seasons[sx].getLow(), -50);
        QCOMPARE(seasons[sx].getAbsoluteStart(), QDate());
        QCOMPARE(seasons[sx].getAbsoluteEnd(), QDate());
        QCOMPARE(seasons[sx].getOffsetStart(), SeasonOffset(1, -3, 1, false));
        QCOMPARE(seasons[sx].getOffsetEnd(), SeasonOffset(1, 1, 0, false));
        QCOMPARE(seasons[sx].isYtd(), false);
        QCOMPARE(seasons[sx].getLength(), SeasonLength());
        QCOMPARE(seasons[sx].events.size(), 0);
        QCOMPARE(seasons[sx].phases.size(), 0);

        ++sx;
        QCOMPARE(seasons[sx].getName(), "6-3 months ago");
        QCOMPARE(seasons[sx].getType(), Season::season);
        QCOMPARE(seasons[sx].id().toString(), "{80348210-8dba-413d-b016-31db8813dffa}");
        QCOMPARE(seasons[sx].getSeed(), 0);
        QCOMPARE(seasons[sx].getLow(), -50);
        QCOMPARE(seasons[sx].getAbsoluteStart(), QDate());
        QCOMPARE(seasons[sx].getAbsoluteEnd(), QDate());
        QCOMPARE(seasons[sx].getOffsetStart(), SeasonOffset(1, -6, 1, false));
        QCOMPARE(seasons[sx].getOffsetEnd(), SeasonOffset());
        QCOMPARE(seasons[sx].isYtd(), false);
        QCOMPARE(seasons[sx].getLength(), SeasonLength(0, 3, 0));
        QCOMPARE(seasons[sx].events.size(), 0);
        QCOMPARE(seasons[sx].phases.size(), 0);

        ++sx;
        QCOMPARE(seasons[sx].getName(), "2023 - YTD");
        QCOMPARE(seasons[sx].getType(), Season::season);
        QCOMPARE(seasons[sx].id().toString(), "{e41cbecf-9dac-41db-bae1-df45877a09ae}");
        QCOMPARE(seasons[sx].getSeed(), 0);
        QCOMPARE(seasons[sx].getLow(), -50);
        QCOMPARE(seasons[sx].getAbsoluteStart(), QDate(2023, 1, 1));
        QCOMPARE(seasons[sx].getAbsoluteEnd(), QDate());
        QCOMPARE(seasons[sx].getOffsetStart(), SeasonOffset());
        QCOMPARE(seasons[sx].getOffsetEnd(), SeasonOffset());
        QCOMPARE(seasons[sx].isYtd(), true);
        QCOMPARE(seasons[sx].getLength(), SeasonLength());
        QCOMPARE(seasons[sx].events.size(), 0);
        QCOMPARE(seasons[sx].phases.size(), 0);

        ++sx;
        QCOMPARE(seasons[sx].getName(), "2023");
        QCOMPARE(seasons[sx].getType(), Season::season);
        QCOMPARE(seasons[sx].id().toString(), "{ed5613a6-875a-483b-a59d-dec36b79fa21}");
        QCOMPARE(seasons[sx].getSeed(), 0);
        QCOMPARE(seasons[sx].getLow(), -50);
        QCOMPARE(seasons[sx].getAbsoluteStart(), QDate(2023, 1, 1));
        QCOMPARE(seasons[sx].getAbsoluteEnd(), QDate());
        QCOMPARE(seasons[sx].getOffsetStart(), SeasonOffset());
        QCOMPARE(seasons[sx].getOffsetEnd(), SeasonOffset());
        QCOMPARE(seasons[sx].isYtd(), false);
        QCOMPARE(seasons[sx].getLength(), SeasonLength(1, 0, 0));
        QCOMPARE(seasons[sx].events.size(), 0);
        QCOMPARE(seasons[sx].phases.size(), 0);

        ++sx;
        QCOMPARE(seasons[sx].getName(), "2024");
        QCOMPARE(seasons[sx].getType(), Season::season);
        QCOMPARE(seasons[sx].id().toString(), "{ea549d73-7d8b-4df4-a16e-0ce50f92d131}");
        QCOMPARE(seasons[sx].getSeed(), 0);
        QCOMPARE(seasons[sx].getLow(), -50);
        QCOMPARE(seasons[sx].getAbsoluteStart(), QDate(2024, 1, 1));
        QCOMPARE(seasons[sx].getAbsoluteEnd(), QDate(2024, 12, 31));
        QCOMPARE(seasons[sx].getOffsetStart(), SeasonOffset());
        QCOMPARE(seasons[sx].getOffsetEnd(), SeasonOffset());
        QCOMPARE(seasons[sx].isYtd(), false);
        QCOMPARE(seasons[sx].getLength(), SeasonLength());
        QCOMPARE(seasons[sx].events.size(), 1);
        QCOMPARE(seasons[sx].events[0].name, "Event");
        QCOMPARE(seasons[sx].events[0].date, QDate(2024, 10, 19));
        QCOMPARE(seasons[sx].events[0].priority, 1);
        QCOMPARE(seasons[sx].events[0].description, "Description");
        QCOMPARE(seasons[sx].events[0].id, "");
        QCOMPARE(seasons[sx].phases.size(), 5);
        QCOMPARE(seasons[sx].phases[0].getName(), "Phase");
        QCOMPARE(seasons[sx].phases[0].getAbsoluteStart(), QDate(2024, 1, 25));
        QCOMPARE(seasons[sx].phases[0].getAbsoluteEnd(), QDate(2024, 1, 31));
        QCOMPARE(seasons[sx].phases[0].getType(), Phase::phase);
        QCOMPARE(seasons[sx].phases[0].id().toString(), "{a3d28e7c-bf42-4a04-8928-c18671c698bc}");
        QCOMPARE(seasons[sx].phases[0].getSeed(), 0);
        QCOMPARE(seasons[sx].phases[0].getLow(), -50);
        QCOMPARE(seasons[sx].phases[1].getName(), "Prep");
        QCOMPARE(seasons[sx].phases[1].getAbsoluteStart(), QDate(2024, 2, 1));
        QCOMPARE(seasons[sx].phases[1].getAbsoluteEnd(), QDate(2024, 2, 10));
        QCOMPARE(seasons[sx].phases[1].getType(), Phase::prep);
        QCOMPARE(seasons[sx].phases[1].id().toString(), "{86489889-ba6e-4006-a7ea-2f9bb7be0620}");
        QCOMPARE(seasons[sx].phases[1].getSeed(), 0);
        QCOMPARE(seasons[sx].phases[1].getLow(), -50);
        QCOMPARE(seasons[sx].phases[2].getName(), "Base");
        QCOMPARE(seasons[sx].phases[2].getAbsoluteStart(), QDate(2024, 3, 1));
        QCOMPARE(seasons[sx].phases[2].getAbsoluteEnd(), QDate(2024, 3, 31));
        QCOMPARE(seasons[sx].phases[2].getType(), Phase::base);
        QCOMPARE(seasons[sx].phases[2].id().toString(), "{f7147da0-de38-41de-9bee-e50562f1dd5f}");
        QCOMPARE(seasons[sx].phases[2].getSeed(), 0);
        QCOMPARE(seasons[sx].phases[2].getLow(), -50);
        QCOMPARE(seasons[sx].phases[3].getName(), "Build");
        QCOMPARE(seasons[sx].phases[3].getAbsoluteStart(), QDate(2024, 4, 1));
        QCOMPARE(seasons[sx].phases[3].getAbsoluteEnd(), QDate(2024, 4, 30));
        QCOMPARE(seasons[sx].phases[3].getType(), Phase::build);
        QCOMPARE(seasons[sx].phases[3].id().toString(), "{98c59d49-65a8-4555-b7c2-8622f24d6091}");
        QCOMPARE(seasons[sx].phases[3].getSeed(), 0);
        QCOMPARE(seasons[sx].phases[3].getLow(), -50);
        QCOMPARE(seasons[sx].phases[4].getName(), "Camp");
        QCOMPARE(seasons[sx].phases[4].getAbsoluteStart(), QDate(2024, 5, 1));
        QCOMPARE(seasons[sx].phases[4].getAbsoluteEnd(), QDate(2024, 5, 31));
        QCOMPARE(seasons[sx].phases[4].getType(), Phase::camp);
        QCOMPARE(seasons[sx].phases[4].id().toString(), "{13fc11c3-4dd1-49df-b927-5d5e33074ee4}");
        QCOMPARE(seasons[sx].phases[4].getSeed(), 0);
        QCOMPARE(seasons[sx].phases[4].getLow(), -50);
    }
};

void TestSeasonParser::failedWritesPreservePreviousFile_data()
{
    QTest::addColumn<int>("failureValue");

    QTest::newRow("open") << static_cast<int>(FailurePoint::Open);
    QTest::newRow("write") << static_cast<int>(FailurePoint::Write);
    QTest::newRow("flush") << static_cast<int>(FailurePoint::Flush);
    QTest::newRow("commit") << static_cast<int>(FailurePoint::Commit);
}

void TestSeasonParser::failedWritesPreservePreviousFile()
{
    QFETCH(int, failureValue);
    const FailurePoint failure = static_cast<FailurePoint>(failureValue);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("seasons.xml"));
    const QByteArray original(
        "<seasons><season><name>existing</name></season></seasons>\n");
    QVERIFY(writeFile(path, original));

    const AtomicFileWriterFactory factory =
        [failure](const QString &, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(failure));
        };
    QString error;

    QVERIFY(!SeasonParser::serialize(path, oneSeason(), &error, factory));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(path), original);
}

void TestSeasonParser::successfulWritePublishesCompleteXml()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("seasons.xml"));
    const QByteArray original("<seasons/>\n");
    QVERIFY(writeFile(path, original));
    QString error;

    QVERIFY(SeasonParser::serialize(path, oneSeason(), &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QByteArray contents = readFile(path);
    QVERIFY(contents.startsWith("<seasons>\n"));
    QVERIFY(contents.endsWith("</seasons>\n"));

    QFile file(path);
    const QList<Season> seasons = SeasonParser::readSeasons(&file);
    QCOMPARE(seasons.size(), 1);
    QCOMPARE(seasons.first().getName(), QStringLiteral("Atomic & <season>"));
    QCOMPARE(seasons.first().getAbsoluteStart(), QDate(2026, 1, 1));
    QCOMPARE(seasons.first().getAbsoluteEnd(), QDate(2026, 12, 31));
}

QTEST_MAIN(TestSeasonParser)
#include "testSeasonParser.moc"
