/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/RealtimeData.h"
#include "Train/TrainingCommandRouter.h"
#include "Train/TrainingCsvSeries.h"

#include <QTest>

class TestVirtualGearRuntime : public QObject
{
    Q_OBJECT

private slots:
    void mapsTrainingKeys_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("command");

        QTest::newRow("space")
                << int(Qt::Key_Space)
                << int(TrainingCommand::ToggleStartPause);
        QTest::newRow("escape")
                << int(Qt::Key_Escape)
                << int(TrainingCommand::RequestStop);
        QTest::newRow("up")
                << int(Qt::Key_Up)
                << int(TrainingCommand::ShiftUp);
        QTest::newRow("w")
                << int(Qt::Key_W)
                << int(TrainingCommand::ShiftUp);
        QTest::newRow("down")
                << int(Qt::Key_Down)
                << int(TrainingCommand::ShiftDown);
        QTest::newRow("s")
                << int(Qt::Key_S)
                << int(TrainingCommand::ShiftDown);
    }

    void mapsTrainingKeys()
    {
        QFETCH(int, key);
        QFETCH(int, command);

        QCOMPARE(int(TrainingCommandRouter::commandForKey(
                         key, Qt::NoModifier, false)), command);
    }

    void leavesUnassignedKeysForTheApplication_data()
    {
        QTest::addColumn<int>("key");
        QTest::newRow("full-screen") << int(Qt::Key_F11);
        QTest::newRow("game-toggle-not-yet-assigned") << int(Qt::Key_G);
        QTest::newRow("letter") << int(Qt::Key_A);
    }

    void leavesUnassignedKeysForTheApplication()
    {
        QFETCH(int, key);
        QCOMPARE(TrainingCommandRouter::commandForKey(
                         key, Qt::NoModifier, false),
                 TrainingCommand::None);
    }

    void rejectsUnsafeModifiedOrRepeatedCommands_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
        QTest::addColumn<bool>("autoRepeat");

        QTest::newRow("control-w")
                << int(Qt::Key_W) << Qt::KeyboardModifiers(Qt::ControlModifier)
                << false;
        QTest::newRow("alt-up")
                << int(Qt::Key_Up) << Qt::KeyboardModifiers(Qt::AltModifier)
                << false;
        QTest::newRow("meta-s")
                << int(Qt::Key_S) << Qt::KeyboardModifiers(Qt::MetaModifier)
                << false;
        QTest::newRow("repeat-down")
                << int(Qt::Key_Down) << Qt::KeyboardModifiers(Qt::NoModifier)
                << true;
    }

    void rejectsUnsafeModifiedOrRepeatedCommands()
    {
        QFETCH(int, key);
        QFETCH(Qt::KeyboardModifiers, modifiers);
        QFETCH(bool, autoRepeat);

        QCOMPARE(TrainingCommandRouter::commandForKey(
                         key, modifiers, autoRepeat),
                 TrainingCommand::None);
    }

    void allowsShiftAndKeypadModifiers()
    {
        QCOMPARE(TrainingCommandRouter::commandForKey(
                         Qt::Key_W, Qt::ShiftModifier, false),
                 TrainingCommand::ShiftUp);
        QCOMPARE(TrainingCommandRouter::commandForKey(
                         Qt::Key_Up, Qt::KeypadModifier, false),
                 TrainingCommand::ShiftUp);
    }

    void realtimeGearIsAnAppendOnlyDisplaySeries()
    {
        RealtimeData data;
        QCOMPARE(data.getVirtualGear(), 0);

        data.setVirtualGear(6);

        QCOMPARE(data.getVirtualGear(), 6);
        QCOMPARE(data.value(RealtimeData::VirtualGear), 6.0);
        QCOMPARE(RealtimeData::listDataSeries().last(),
                 RealtimeData::VirtualGear);
        QCOMPARE(RealtimeData::seriesName(RealtimeData::VirtualGear),
                 QStringLiteral("Virtual Gear"));
        QCOMPARE(RealtimeData::seriesSymbol(RealtimeData::VirtualGear),
                 QStringLiteral("Virtual Gear"));
    }

    void legacyTargetOnlyCsvKeepsItsOriginalSeriesShape()
    {
        const TrainingCsvSeriesLayout layout =
                TrainingCsvSeriesLayout::fromColumns({
                    QStringLiteral("secs"), QStringLiteral("target")
                });

        QCOMPARE(layout.valueNames(),
                 QStringList({QStringLiteral("TARGET")}));
        QCOMPARE(layout.unitNames(),
                 QStringList({QStringLiteral("Watts")}));
        QVERIFY(layout.shouldAppend(235.0, 0.0));
        QVERIFY(!layout.shouldAppend(0.0, 0.0));
        QCOMPARE(layout.values(235.0, 0.0), QVector<double>({235.0}));
    }

    void virtualGearCsvAddsBackwardCompatibleTrainSeriesValue()
    {
        const TrainingCsvSeriesLayout layout =
                TrainingCsvSeriesLayout::fromColumns({
                    QStringLiteral("secs"),
                    QStringLiteral("target"),
                    QStringLiteral("virtualgear")
                });

        QCOMPARE(layout.valueNames(), QStringList({
                QStringLiteral("TARGET"),
                QStringLiteral("VIRTUAL_GEAR")
        }));
        QCOMPARE(layout.unitNames(), QStringList({
                QStringLiteral("Watts"),
                QStringLiteral("Gear")
        }));
        QVERIFY(layout.shouldAppend(0.0, 6.0));
        QCOMPARE(layout.values(0.0, 6.0),
                 QVector<double>({0.0, 6.0}));
    }

    void gearOnlyAndUnrelatedCsvSchemasAreHandled()
    {
        const TrainingCsvSeriesLayout gearOnly =
                TrainingCsvSeriesLayout::fromColumns({
                    QStringLiteral("secs"), QStringLiteral("virtualgear")
                });
        QCOMPARE(gearOnly.valueNames(),
                 QStringList({QStringLiteral("VIRTUAL_GEAR")}));
        QCOMPARE(gearOnly.values(300.0, 7.0), QVector<double>({7.0}));

        const TrainingCsvSeriesLayout unrelated =
                TrainingCsvSeriesLayout::fromColumns({
                    QStringLiteral("secs"), QStringLiteral("watts")
                });
        QVERIFY(unrelated.isEmpty());
        QVERIFY(!unrelated.shouldAppend(300.0, 7.0));
        QVERIFY(unrelated.values(300.0, 7.0).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestVirtualGearRuntime)
#include "testVirtualGearRuntime.moc"
