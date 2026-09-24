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
#include <QAction>
#include <QPointer>
#include <QPropertyAnimation>
#include <memory>

#include "ChartSpace.h"
#include "Context.h"
#include "Overview.h"
#include "Settings.h"

class TestViewOwnership : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Production static initialization precedes main; use the isolated
        // chartOwnership launcher before starting this executable.
        const QString root = qEnvironmentVariable("GC_CHART_OWNERSHIP_ROOT");
        QVERIFY2(!root.isEmpty(), "Use the isolated chartOwnership launcher");
        QCOMPARE(qEnvironmentVariable("XDG_CONFIG_HOME"), root + "/config");
        QVERIFY(appsettings);
        GCColor::setupColors();
    }

    void overviewDeletesItsActions()
    {
        Context context(nullptr);
        // Exercise the actual constructor without showing the window or
        // configuring tiles. No athlete or replacement owner is supplied.
        auto overview = std::make_unique<OverviewWindow>(
                &context, OverviewScope::ANALYSIS, true);
        QCOMPARE(overview->actions.size(), 3);

        QList<QPointer<QAction>> actions;
        for (QAction *action : overview->actions)
            actions.append(action);

        overview.reset();
        bool allDeleted = true;
        for (const QPointer<QAction> &action : actions) {
            allDeleted &= action.isNull();
            // Preserve a useful differential failure without leaving the
            // old parentless actions alive for the rest of the test process.
            if (action) delete action.data();
        }
        QVERIFY2(allDeleted, "OverviewWindow must delete its three actions");
    }

    void chartSpaceOwnsPersistentAnimations()
    {
        Context context(nullptr);
        auto space = std::make_unique<ChartSpace>(
                &context, OverviewScope::ANALYSIS, nullptr);
        const auto animations = space->findChildren<QPropertyAnimation *>(
                QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(animations.size(), 2);

        QList<QPointer<QPropertyAnimation>> probes;
        QList<QByteArray> properties;
        for (QPropertyAnimation *animation : animations) {
            QCOMPARE(animation->targetObject(), static_cast<QObject *>(space.get()));
            probes.append(animation);
            properties.append(animation->propertyName());
        }
        QVERIFY(properties.contains(QByteArray("viewRect")));
        QVERIFY(properties.contains(QByteArray("viewY")));

        space.reset();
        for (const QPointer<QPropertyAnimation> &animation : probes)
            QVERIFY2(animation.isNull(), "ChartSpace animation outlived its owner");
    }
};

QTEST_MAIN(TestViewOwnership)
#include "testViewOwnership.moc"
