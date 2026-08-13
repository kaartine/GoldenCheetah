/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "GuiStartupPolicy.h"

#include <QTest>

class TestGuiStartupPolicy : public QObject
{
    Q_OBJECT

private slots:
    void offscreenSkipsWebEnginePrimer_data();
    void offscreenSkipsWebEnginePrimer();
    void desktopPlatformUsesWebEnginePrimer_data();
    void desktopPlatformUsesWebEnginePrimer();
};

void TestGuiStartupPolicy::offscreenSkipsWebEnginePrimer_data()
{
    QTest::addColumn<QString>("platformName");

    QTest::newRow("lowercase") << QStringLiteral("offscreen");
    QTest::newRow("mixed-case") << QStringLiteral("OffScreen");
}

void TestGuiStartupPolicy::offscreenSkipsWebEnginePrimer()
{
    QFETCH(QString, platformName);

    QVERIFY(!GuiStartupPolicy::shouldPrimeWebEngine(platformName));
}

void TestGuiStartupPolicy::desktopPlatformUsesWebEnginePrimer_data()
{
    QTest::addColumn<QString>("platformName");

    QTest::newRow("xcb") << QStringLiteral("xcb");
    QTest::newRow("wayland") << QStringLiteral("wayland");
    QTest::newRow("windows") << QStringLiteral("windows");
    QTest::newRow("cocoa") << QStringLiteral("cocoa");
}

void TestGuiStartupPolicy::desktopPlatformUsesWebEnginePrimer()
{
    QFETCH(QString, platformName);

    QVERIFY(GuiStartupPolicy::shouldPrimeWebEngine(platformName));
}

QTEST_GUILESS_MAIN(TestGuiStartupPolicy)

#include "testGuiStartupPolicy.moc"
