#include <QtTest>

#include "AtomicFileWriter.h"
#include "RideCachePersistence.h"

#include <QFile>
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
        return QStringLiteral("injected cache write failure");
    }

private:
    FailurePoint failure_;
};

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(data) == data.size()
        && file.flush();
}

} // namespace

class TestRideCacheAtomicSave : public QObject
{
    Q_OBJECT

private slots:
    void failedWritesPreservePreviousCache_data();
    void failedWritesPreservePreviousCache();
    void successfulWritePublishesCompleteCache();
};

void TestRideCacheAtomicSave::failedWritesPreservePreviousCache_data()
{
    QTest::addColumn<int>("failureValue");

    QTest::newRow("open") << static_cast<int>(FailurePoint::Open);
    QTest::newRow("write") << static_cast<int>(FailurePoint::Write);
    QTest::newRow("flush") << static_cast<int>(FailurePoint::Flush);
    QTest::newRow("commit") << static_cast<int>(FailurePoint::Commit);
}

void TestRideCacheAtomicSave::failedWritesPreservePreviousCache()
{
    QFETCH(int, failureValue);
    const FailurePoint failure = static_cast<FailurePoint>(failureValue);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("rideDB.json"));
    const QByteArray original("{\"VERSION\":\"existing\"}\n");
    const QByteArray replacement("{\"VERSION\":\"replacement\"}\n");
    QVERIFY(writeFile(path, original));

    const AtomicFileWriterFactory factory =
        [failure](const QString &, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(failure));
        };
    QString error;

    QVERIFY(!writeRideCacheAtomically(
        path, replacement, error, factory));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(path), original);
}

void TestRideCacheAtomicSave::successfulWritePublishesCompleteCache()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("rideDB.json"));
    const QByteArray original("{\"VERSION\":\"existing\"}\n");
    const QByteArray replacement(
        "\xEF\xBB\xBF{\n  \"VERSION\":\"replacement\",\n"
        "  \"RIDES\":[]\n}\n");
    QVERIFY(writeFile(path, original));
    QString error;

    QVERIFY2(
        writeRideCacheAtomically(path, replacement, error),
        qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(readFile(path), replacement);
}

QTEST_MAIN(TestRideCacheAtomicSave)
#include "testRideCacheAtomicSave.moc"
