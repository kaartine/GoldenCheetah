#include <QtTest>

#include "AtomicFileWriter.h"
#include "Measures.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

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

MeasuresGroup makeMeasuresGroup(const QString &directory)
{
    return MeasuresGroup(
        QStringLiteral("Body"),
        QStringLiteral("Body"),
        QStringList({QStringLiteral("Weight")}),
        QStringList({QStringLiteral("Weight")}),
        QStringList({QStringLiteral("kg")}),
        QStringList({QStringLiteral("lb")}),
        QList<double>({1.0}),
        QList<QStringList>({QStringList()}),
        QDir(directory),
        true);
}

void setOneMeasure(MeasuresGroup &group)
{
    Measure measure;
    measure.when = QDateTime::fromSecsSinceEpoch(
        1783405800, QTimeZone::UTC);
    measure.comment = QStringLiteral("atomic");
    measure.values[0] = 72.5;
    QList<Measure> measures({measure});
    group.setMeasures(measures);
}

} // namespace

class TestMeasuresAtomicSave : public QObject
{
    Q_OBJECT

private slots:
    void failedWritesPreservePreviousFile_data();
    void failedWritesPreservePreviousFile();
    void successfulWritePublishesCompleteJson();
};

void TestMeasuresAtomicSave::failedWritesPreservePreviousFile_data()
{
    QTest::addColumn<int>("failureValue");

    QTest::newRow("open") << static_cast<int>(FailurePoint::Open);
    QTest::newRow("write") << static_cast<int>(FailurePoint::Write);
    QTest::newRow("flush") << static_cast<int>(FailurePoint::Flush);
    QTest::newRow("commit") << static_cast<int>(FailurePoint::Commit);
}

void TestMeasuresAtomicSave::failedWritesPreservePreviousFile()
{
    QFETCH(int, failureValue);
    const FailurePoint failure = static_cast<FailurePoint>(failureValue);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path =
        directory.filePath(QStringLiteral("bodymeasures.json"));
    const QByteArray original("{\"version\":1,\"measures\":[]}\n");
    QVERIFY(writeFile(path, original));

    MeasuresGroup group = makeMeasuresGroup(directory.path());
    setOneMeasure(group);
    const AtomicFileWriterFactory factory =
        [failure](const QString &, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(failure));
        };
    QString error;

    QVERIFY(!group.write(&error, factory));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(path), original);
}

void TestMeasuresAtomicSave::successfulWritePublishesCompleteJson()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path =
        directory.filePath(QStringLiteral("bodymeasures.json"));
    const QByteArray original("{\"version\":1,\"measures\":[]}\n");
    QVERIFY(writeFile(path, original));

    MeasuresGroup group = makeMeasuresGroup(directory.path());
    setOneMeasure(group);
    QString error;

    QVERIFY(group.write(&error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonDocument document =
        QJsonDocument::fromJson(readFile(path));
    QVERIFY(document.isObject());
    QCOMPARE(
        document.object().value(QStringLiteral("version")).toInt(),
        1);
    const QJsonArray measures =
        document.object().value(QStringLiteral("measures")).toArray();
    QCOMPARE(measures.size(), 1);
    QCOMPARE(
        measures.at(0).toObject()
            .value(QStringLiteral("weight")).toDouble(),
        72.5);
}

QTEST_MAIN(TestMeasuresAtomicSave)
#include "testMeasuresAtomicSave.moc"
