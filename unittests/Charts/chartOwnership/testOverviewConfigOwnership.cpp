/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <QtTest>
#include <QPointer>
#include <memory>

#include "ChartSpace.h"
#include "Context.h"
#include "Overview.h"
#include "OverviewItems.h"
#include "Settings.h"
#include "UserChartOverviewItem.h"

class TestOverviewConfigOwnership : public QObject
{
    Q_OBJECT

private:
    // Selects the tile subclass under test so dialog lifecycle cases can be
    // exercised against both a plain ChartSpaceItem (RPE) and the UserChart
    // tile, which owns an extra top-level config widget.
    static ChartSpaceItem *makeTile(ChartSpace *space, bool userChart, const QString &name = "Ownership test")
    {
        if (userChart) return new UserChartOverviewItem(space, name, "{}");
        return new RPEOverviewItem(space, name);
    }

private slots:
    void initTestCase()
    {
        const QString root = qEnvironmentVariable("GC_CHART_OWNERSHIP_ROOT");
        QVERIFY2(!root.isEmpty(), "Use the isolated chartOwnership launcher");
        QCOMPARE(qEnvironmentVariable("XDG_CONFIG_HOME"), root + "/config");
        QVERIFY(appsettings);
        GCColor::setupColors();
    }

    void tileDeletesConfig()
    {
        Context context(nullptr);
        ChartSpace space(&context, OverviewScope::ANALYSIS, nullptr);
        auto tile = std::make_unique<RPEOverviewItem>(&space, "Ownership test");
        QPointer<QWidget> config(tile->config());
        QVERIFY(config);
        QVERIFY(!config->parent());
        tile.reset();
        const bool deleted = config.isNull();
        // Clean up a surviving parentless widget on the safe RED path.
        delete config.data();
        QVERIFY2(deleted, "Tile destruction must delete its config widget");
    }

    void dialogReleasePreservesConfig_data()
    {
        QTest::addColumn<bool>("closeFirst");
        QTest::addColumn<bool>("userChart");
        QTest::newRow("rpe-close") << true << false;
        QTest::newRow("rpe-destruction") << false << false;
        QTest::newRow("userchart-close") << true << true;
        QTest::newRow("userchart-destruction") << false << true;
    }

    void dialogReleasePreservesConfig()
    {
        QFETCH(bool, closeFirst);
        QFETCH(bool, userChart);
        Context context(nullptr);
        ChartSpace space(&context, OverviewScope::ANALYSIS, nullptr);
        std::unique_ptr<ChartSpaceItem> tile(makeTile(&space, userChart));
        QPointer<QWidget> config(tile->config());
        QPointer<OverviewConfigDialog> dialog(new OverviewConfigDialog(tile.get(), QPoint()));
        // Never show this dialog: the real showEvent requires a MainWindow.
        QCOMPARE(config->parentWidget(), static_cast<QWidget *>(dialog.data()));
        if (closeFirst) dialog->close();
        delete dialog.data();
        QVERIFY(config);
        QVERIFY(!config->parent());
        QVERIFY(config->isHidden());
        tile.reset();
        const bool deleted = config.isNull();
        delete config.data();
        QVERIFY2(deleted, "Detached config must remain owned by its tile");
    }

    void dialogRemoveDeletesTileAndConfig_data()
    {
        QTest::addColumn<bool>("userChart");
        QTest::newRow("rpe") << false;
        QTest::newRow("userchart") << true;
    }

    void dialogRemoveDeletesTileAndConfig()
    {
        QFETCH(bool, userChart);
        Context context(nullptr);
        ChartSpace space(&context, OverviewScope::ANALYSIS, nullptr);
        QPointer<ChartSpaceItem> tile(makeTile(&space, userChart));
        space.addItem(0, 0, 1, 5, tile.data());
        space.getScene()->addItem(tile.data());
        QPointer<QWidget> config(tile->config());
        QPointer<OverviewConfigDialog> dialog(new OverviewConfigDialog(tile.data(), QPoint()));
        dialog->removeItem();
        const bool tileDeleted = tile.isNull();
        const bool configDeleted = config.isNull();
        // Old code leaves config parented to the dialog until this cleanup.
        delete dialog.data();
        QVERIFY2(tileDeleted, "Remove must destroy the registered tile");
        QVERIFY2(configDeleted, "Remove must release config with its tile");
    }

    void externalRemovalLeavesDialogSafe_data()
    {
        QTest::addColumn<bool>("userChart");
        QTest::newRow("rpe") << false;
        QTest::newRow("userchart") << true;
    }

    void externalRemovalLeavesDialogSafe()
    {
        QFETCH(bool, userChart);
        Context context(nullptr);
        ChartSpace space(&context, OverviewScope::ANALYSIS, nullptr);
        QPointer<ChartSpaceItem> tile(makeTile(&space, userChart));
        space.addItem(0, 0, 1, 5, tile.data());
        space.getScene()->addItem(tile.data());
        QPointer<QWidget> config(tile->config());
        QPointer<OverviewConfigDialog> dialog(new OverviewConfigDialog(tile.data(), QPoint()));
        space.removeItem(tile.data());
        QVERIFY(tile.isNull());
        QVERIFY(config.isNull());
        dialog->removeItem();
        if (dialog) dialog->close();
        delete dialog.data();
        QVERIFY(dialog.isNull());
    }

    void userChartTileDeletesConfig()
    {
        Context context(nullptr);
        ChartSpace space(&context, OverviewScope::ANALYSIS, nullptr);
        auto tile = std::make_unique<UserChartOverviewItem>(&space, "Ownership test", "{}");
        QPointer<QWidget> config(tile->config());
        QVERIFY(config);
        tile.reset();
        const bool deleted = config.isNull();
        delete config.data(); // Only needed while exercising the old object.
        QVERIFY2(deleted, "User chart tile must release its config widget");
    }

    void userChartSceneDeletesConfig()
    {
        Context context(nullptr);
        auto space = std::make_unique<ChartSpace>(&context, OverviewScope::ANALYSIS, nullptr);
        auto *tile = new UserChartOverviewItem(space.get(), "Ownership test", "{}");
        space->addItem(0, 0, 2, 5, tile);
        space->getScene()->addItem(tile);
        QPointer<QWidget> config(tile->config());
        QVERIFY(config);
        space.reset();
        const bool deleted = config.isNull();
        delete config.data(); // Only needed while exercising the old object.
        QVERIFY2(deleted, "Scene teardown must release user chart config");
    }

    void userChartProxyFirstDeletesConfig()
    {
        // proxy is a top-level scene item (QObject child only), so scene
        // teardown may delete it - and chart - before the tile; force that
        // order.
        Context context(nullptr);
        ChartSpace space(&context, OverviewScope::ANALYSIS, nullptr);
        auto tile = std::make_unique<UserChartOverviewItem>(&space, "Ownership test", "{}");
        QPointer<QWidget> config(tile->config());
        QVERIFY(config);
        QPointer<QGraphicsProxyWidget> proxy(tile->proxy);
        QVERIFY(proxy);
        space.getScene()->removeItem(proxy.data());
        delete proxy.data();
        QVERIFY2(proxy.isNull(), "Test setup must actually delete the proxy before the tile");
        tile.reset();
        const bool deleted = config.isNull();
        delete config.data(); // Only needed while exercising the old object.
        QVERIFY2(deleted, "Tile destructor must release config even if its chart proxy died first");
    }
};

QTEST_MAIN(TestOverviewConfigOwnership)
#include "testOverviewConfigOwnership.moc"
