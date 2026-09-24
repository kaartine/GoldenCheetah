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
#include <QFile>

class TestGuiStartupPolicy : public QObject
{
    Q_OBJECT

private slots:
    void offscreenSkipsWebEnginePrimer_data();
    void offscreenSkipsWebEnginePrimer();
    void desktopPlatformUsesWebEnginePrimer_data();
    void desktopPlatformUsesWebEnginePrimer();
    void pythonStartupOwnsFailedInitialization();
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

void TestGuiStartupPolicy::pythonStartupOwnsFailedInitialization()
{
    // Source-contract coverage for main's ownership wiring, not a simulated
    // CPython runtime test. Loading the optional interpreter is a separate gate.
    QFile mainSource(QFINDTESTDATA("../../../src/Core/main.cpp"));
    QVERIFY(mainSource.open(QIODevice::ReadOnly));
    const QByteArray source = mainSource.readAll();
    const auto begin = source.indexOf("if (embed && noPy == false && python == NULL)");
    QVERIFY(begin >= 0);
    const auto end = source.indexOf("#endif", begin);
    QVERIFY(end > begin);
    const QByteArray initialization = source.mid(begin, end - begin);
    QVERIFY(initialization.contains("auto candidate = std::make_unique<PythonEmbed>();"));
    QVERIFY(initialization.contains("if (candidate->loaded) python = candidate.release();"));
    QVERIFY(!initialization.contains("python = new PythonEmbed"));
}

QTEST_GUILESS_MAIN(TestGuiStartupPolicy)

#include "testGuiStartupPolicy.moc"
