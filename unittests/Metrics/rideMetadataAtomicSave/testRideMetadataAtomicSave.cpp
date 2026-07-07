#include <QtTest>

#include "AtomicFileWriter.h"
#include "RideMetadata.h"

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

QList<KeywordDefinition> keywords()
{
    KeywordDefinition keyword;
    keyword.name = QStringLiteral("Tempo & <threshold>");
    keyword.color = QColor(12, 34, 56);
    keyword.tokens = QStringList({
        QStringLiteral("steady & strong"),
        QStringLiteral("zone <four>")
    });
    return QList<KeywordDefinition>({keyword});
}

QList<FieldDefinition> fields()
{
    return QList<FieldDefinition>({FieldDefinition(
        QStringLiteral("Workout"),
        QStringLiteral("Plan & notes"),
        GcFieldType::FIELD_TEXTBOX,
        true,
        false,
        QStringList({QStringLiteral("easy"), QStringLiteral("hard")}),
        QStringLiteral("Bike < Run & Swim"))});
}

QList<DefaultDefinition> defaults()
{
    return QList<DefaultDefinition>({DefaultDefinition(
        QStringLiteral("Plan & notes"),
        QStringLiteral("steady <effort>"),
        QStringLiteral("Sport"),
        QStringLiteral("Bike & Indoor"))});
}

const FieldDefinition *findField(
    const QList<FieldDefinition> &definitions, const QString &name)
{
    for (const FieldDefinition &definition : definitions) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

} // namespace

class TestRideMetadataAtomicSave : public QObject
{
    Q_OBJECT

private slots:
    void failedWritesPreservePreviousFile_data();
    void failedWritesPreservePreviousFile();
    void successfulWritePublishesCompleteXml();
};

void TestRideMetadataAtomicSave::failedWritesPreservePreviousFile_data()
{
    QTest::addColumn<int>("failureValue");

    QTest::newRow("open") << static_cast<int>(FailurePoint::Open);
    QTest::newRow("write") << static_cast<int>(FailurePoint::Write);
    QTest::newRow("flush") << static_cast<int>(FailurePoint::Flush);
    QTest::newRow("commit") << static_cast<int>(FailurePoint::Commit);
}

void TestRideMetadataAtomicSave::failedWritesPreservePreviousFile()
{
    QFETCH(int, failureValue);
    const FailurePoint failure = static_cast<FailurePoint>(failureValue);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("metadata.xml"));
    const QByteArray original(
        "<metadata><colorfield>existing</colorfield></metadata>\n");
    QVERIFY(writeFile(path, original));

    const AtomicFileWriterFactory factory =
        [failure](const QString &, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(failure));
        };
    QString error;

    QVERIFY(!RideMetadata::serialize(
        path,
        keywords(),
        fields(),
        QStringLiteral("Calendar & highlight"),
        defaults(),
        &error,
        factory));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(path), original);
}

void TestRideMetadataAtomicSave::successfulWritePublishesCompleteXml()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("metadata.xml"));
    const QByteArray original("<metadata/>\n");
    QVERIFY(writeFile(path, original));
    QString error;

    QVERIFY(RideMetadata::serialize(
        path,
        keywords(),
        fields(),
        QStringLiteral("Calendar & highlight"),
        defaults(),
        &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QByteArray contents = readFile(path);
    QVERIFY(contents.startsWith("<metadata>\n"));
    QVERIFY(contents.endsWith("</metadata>\n"));

    QList<KeywordDefinition> parsedKeywords;
    QList<FieldDefinition> parsedFields;
    QList<DefaultDefinition> parsedDefaults;
    QString parsedColorField;
    RideMetadata::readXML(
        path,
        parsedKeywords,
        parsedFields,
        parsedColorField,
        parsedDefaults);

    QCOMPARE(parsedColorField, QStringLiteral("Calendar & highlight"));
    QCOMPARE(parsedKeywords.size(), 1);
    QCOMPARE(parsedKeywords.first().name,
             QStringLiteral("Tempo & <threshold>"));
    QCOMPARE(parsedKeywords.first().color, QColor(12, 34, 56));
    QCOMPARE(parsedKeywords.first().tokens,
             QStringList({QStringLiteral("steady & strong"),
                          QStringLiteral("zone <four>")}));

    const FieldDefinition *field =
        findField(parsedFields, QStringLiteral("Plan & notes"));
    QVERIFY(field != nullptr);
    QCOMPARE(field->tab, QStringLiteral("Workout"));
    QCOMPARE(field->type, GcFieldType::FIELD_TEXTBOX);
    QCOMPARE(field->diary, true);
    QCOMPARE(field->interval, false);
    QCOMPARE(field->values,
             QStringList({QStringLiteral("easy"), QStringLiteral("hard")}));
    QCOMPARE(field->expression, QStringLiteral("Bike < Run & Swim"));

    QCOMPARE(parsedDefaults.size(), 1);
    QCOMPARE(parsedDefaults.first().field, QStringLiteral("Plan & notes"));
    QCOMPARE(parsedDefaults.first().value,
             QStringLiteral("steady <effort>"));
    QCOMPARE(parsedDefaults.first().linkedField, QStringLiteral("Sport"));
    QCOMPARE(parsedDefaults.first().linkedValue,
             QStringLiteral("Bike & Indoor"));
}

QTEST_MAIN(TestRideMetadataAtomicSave)
#include "testRideMetadataAtomicSave.moc"
