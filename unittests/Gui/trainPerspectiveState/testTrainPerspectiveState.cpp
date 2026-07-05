/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Gui/TrainPerspectiveState.h"

#include <QDomDocument>
#include <QTest>

class TestTrainPerspectiveState : public QObject
{
    Q_OBJECT

    static QString dial(int series, const QString &width, const QString &title = "Metric")
    {
        return QString(
            "<chart id=\"22\" title=\"%1\">"
            "<property name=\"widthFactor\" type=\"double\" value=\"%2\"/>"
            "<property name=\"showInstant\" type=\"bool\" value=\"1\"/>"
            "<property name=\"dataSeries\" type=\"int\" value=\"%3\"/>"
            "</chart>")
            .arg(title, width, QString::number(series));
    }

    static QString layout(const QList<int> &series, const QString &version = QString())
    {
        QString charts;
        foreach (int value, series) charts += dial(value, "6.25");

        const QString versionAttribute =
            version.isEmpty() ? QString() : QString(" version=\"%1\"").arg(version);
        return QString(
            "<layouts%1>"
            "<layout name=\"Legacy\" trainswitch=\"1\">"
            "%2"
            "<chart id=\"36\" title=\"Workout\"/>"
            "</layout>"
            "</layouts>")
            .arg(versionAttribute, charts);
    }

    static QString defaults()
    {
        return QString(
            "<layouts>"
            "<layout name=\"Erg Workout\" trainswitch=\"1\">"
            "%1"
            "</layout>"
            "</layouts>")
            .arg(dial(8, "7.14286", "Heart Rate"));
    }

    static QDomElement property(const QDomElement &chart, const QString &name)
    {
        for (QDomElement candidate = chart.firstChildElement("property");
             !candidate.isNull();
             candidate = candidate.nextSiblingElement("property")) {
            if (candidate.attribute("name") == name) return candidate;
        }
        return QDomElement();
    }

    static QDomDocument parse(const QString &content)
    {
        QDomDocument document;
        if (!document.setContent(content)) return QDomDocument();
        return document;
    }

    static QList<QDomElement> telemetryCharts(const QDomDocument &document)
    {
        QList<QDomElement> result;
        const QDomElement layout = document.documentElement().firstChildElement("layout");
        for (QDomElement chart = layout.firstChildElement("chart");
             !chart.isNull() && chart.attribute("id") == "22";
             chart = chart.nextSiblingElement("chart")) {
            result.append(chart);
        }
        return result;
    }

    static QList<int> telemetrySeries(const QDomDocument &document)
    {
        QList<int> result;
        foreach (const QDomElement &chart, telemetryCharts(document)) {
            result.append(property(chart, "dataSeries").attribute("value").toInt());
        }
        return result;
    }

private slots:

    void migratesLegacyDefault()
    {
        const QList<int> oldDefault = QList<int>() << 5 << 9 << 12 << 19 << 6 << 7;
        QString content = layout(oldDefault);

        QVERIFY(TrainPerspectiveState::migrate(content, defaults()));

        const QDomDocument document = parse(content);
        QVERIFY(!document.isNull());
        QCOMPARE(document.documentElement().attribute("version"), QString("2"));

        const QList<int> expected = QList<int>() << 5 << 9 << 12 << 19 << 6 << 7 << 8;
        QCOMPARE(telemetrySeries(document), expected);

        const QList<QDomElement> charts = telemetryCharts(document);
        QCOMPARE(charts.count(), 7);
        foreach (const QDomElement &chart, charts) {
            QCOMPARE(property(chart, "widthFactor").attribute("value"), QString("7.14286"));
        }
        QCOMPARE(charts.last().attribute("title"), QString("Heart Rate"));
        QCOMPARE(charts.last().nextSiblingElement("chart").attribute("id"), QString("36"));
    }

    void preservesCustomizedLayout()
    {
        const QList<int> customized = QList<int>() << 5 << 9 << 10 << 19 << 6 << 7;
        QString content = layout(customized);

        QVERIFY(TrainPerspectiveState::migrate(content, defaults()));

        const QDomDocument document = parse(content);
        QCOMPARE(document.documentElement().attribute("version"), QString("2"));
        QCOMPARE(telemetrySeries(document), customized);
        foreach (const QDomElement &chart, telemetryCharts(document)) {
            QCOMPARE(property(chart, "widthFactor").attribute("value"), QString("6.25"));
        }
    }

    void respectsCurrentVersion()
    {
        const QList<int> oldDefault = QList<int>() << 5 << 9 << 12 << 19 << 6 << 7;
        QString content = layout(oldDefault, "2");
        const QString original = content;

        QVERIFY(!TrainPerspectiveState::migrate(content, defaults()));
        QCOMPARE(content, original);
    }

    void doesNotDuplicateExistingHeartRate()
    {
        const QList<int> withHeartRate = QList<int>() << 5 << 9 << 12 << 19 << 6 << 7 << 8;
        QString content = layout(withHeartRate);

        QVERIFY(TrainPerspectiveState::migrate(content, defaults()));

        const QDomDocument document = parse(content);
        QCOMPARE(telemetrySeries(document), withHeartRate);
        QCOMPARE(document.documentElement().attribute("version"), QString("2"));
    }
};

QTEST_MAIN(TestTrainPerspectiveState)
#include "testTrainPerspectiveState.moc"
