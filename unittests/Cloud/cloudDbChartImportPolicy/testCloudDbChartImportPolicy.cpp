#include "Cloud/CloudDBChartImportPolicy.h"
#include "Gui/GcWindowTypes.h"

#include <QTest>

class TestCloudDBChartImportPolicy : public QObject
{
    Q_OBJECT

private slots:
    void rejectsMissingChartDefinitions();
    void rejectsMalformedTypes_data();
    void rejectsMalformedTypes();
    void rejectsExecutableTypes_data();
    void rejectsExecutableTypes();
    void rejectsExecutableProperties_data();
    void rejectsExecutableProperties();
    void rejectsMixedImportsAtomically();
    void acceptsOrdinaryCharts();
};

namespace {

QMap<QString, QString> chartProperties(int type)
{
    return {
        {QStringLiteral("TYPE"), QString::number(type)},
        {QStringLiteral("VIEW"), QStringLiteral("0")},
        {QStringLiteral("title"), QStringLiteral("Weekly overview")}
    };
}

} // namespace

void TestCloudDBChartImportPolicy::rejectsMissingChartDefinitions()
{
    QCOMPARE(CloudDBChartImportPolicy::evaluate({}),
             CloudDBChartImportPolicy::Decision::RejectMalformed);
}

void TestCloudDBChartImportPolicy::rejectsMalformedTypes_data()
{
    QTest::addColumn<QString>("type");
    QTest::newRow("missing") << QString();
    QTest::newRow("text") << QStringLiteral("Python");
    QTest::newRow("suffix") << QStringLiteral("43x");
    QTest::newRow("negative") << QStringLiteral("-1");
}

void TestCloudDBChartImportPolicy::rejectsMalformedTypes()
{
    QFETCH(QString, type);
    QMap<QString, QString> properties = chartProperties(GcWindowTypes::Overview);
    if (type.isNull()) properties.remove(QStringLiteral("TYPE"));
    else properties[QStringLiteral("TYPE")] = type;

    QCOMPARE(CloudDBChartImportPolicy::evaluate({properties}),
             CloudDBChartImportPolicy::Decision::RejectMalformed);
}

void TestCloudDBChartImportPolicy::rejectsExecutableTypes_data()
{
    QTest::addColumn<int>("type");
    QTest::newRow("r-activity") << int(GcWindowTypes::RConsole);
    QTest::newRow("r-season") << int(GcWindowTypes::RConsoleSeason);
    QTest::newRow("python-activity") << int(GcWindowTypes::Python);
    QTest::newRow("python-season") << int(GcWindowTypes::PythonSeason);
}

void TestCloudDBChartImportPolicy::rejectsExecutableTypes()
{
    QFETCH(int, type);
    QCOMPARE(CloudDBChartImportPolicy::evaluate({chartProperties(type)}),
             CloudDBChartImportPolicy::Decision::RejectExecutableType);
}

void TestCloudDBChartImportPolicy::rejectsExecutableProperties_data()
{
    QTest::addColumn<QString>("propertyName");
    QTest::newRow("lowercase") << QStringLiteral("script");
    QTest::newRow("uppercase") << QStringLiteral("SCRIPT");
    QTest::newRow("mixed-case") << QStringLiteral("Script");
}

void TestCloudDBChartImportPolicy::rejectsExecutableProperties()
{
    QFETCH(QString, propertyName);
    QMap<QString, QString> properties = chartProperties(GcWindowTypes::Overview);
    properties[propertyName] = QStringLiteral("malicious-code");

    QCOMPARE(CloudDBChartImportPolicy::evaluate({properties}),
             CloudDBChartImportPolicy::Decision::RejectExecutableProperty);
}

void TestCloudDBChartImportPolicy::rejectsMixedImportsAtomically()
{
    const QList<QMap<QString, QString>> properties = {
        chartProperties(GcWindowTypes::Overview),
        chartProperties(GcWindowTypes::Python)
    };

    QCOMPARE(CloudDBChartImportPolicy::evaluate(properties),
             CloudDBChartImportPolicy::Decision::RejectExecutableType);
}

void TestCloudDBChartImportPolicy::acceptsOrdinaryCharts()
{
    const QList<QMap<QString, QString>> properties = {
        chartProperties(GcWindowTypes::Overview),
        chartProperties(GcWindowTypes::Histogram)
    };

    QCOMPARE(CloudDBChartImportPolicy::evaluate(properties),
             CloudDBChartImportPolicy::Decision::Allow);
}

QTEST_MAIN(TestCloudDBChartImportPolicy)
#include "testCloudDbChartImportPolicy.moc"
