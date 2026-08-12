/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "GuiSmokeShutdown.h"

#include <QCoreApplication>
#include <QEvent>
#include <QTest>

class TestGuiSmokeShutdown : public QObject
{
    Q_OBJECT

private slots:
    void acceptedCloseWaitsForDeferredDestruction();
    void rejectedCloseExitsFailureOnlyOnce();
    void synchronousDestructionExitsSuccess();
    void invalidInputsFailWithoutDuplicateExit();
};

void TestGuiSmokeShutdown::acceptedCloseWaitsForDeferredDestruction()
{
    QObject *window = new QObject;
    QList<int> exitCodes;

    QVERIFY(GuiSmokeShutdown::complete(
        window,
        [window]() {
            window->deleteLater();
            return true;
        },
        [&exitCodes](int code) { exitCodes.append(code); },
        0,
        1));
    QVERIFY(exitCodes.isEmpty());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QCOMPARE(exitCodes, QList<int>({0}));
}

void TestGuiSmokeShutdown::rejectedCloseExitsFailureOnlyOnce()
{
    QObject *window = new QObject;
    QList<int> exitCodes;

    QVERIFY(!GuiSmokeShutdown::complete(
        window,
        []() { return false; },
        [&exitCodes](int code) { exitCodes.append(code); },
        0,
        1));
    QCOMPARE(exitCodes, QList<int>({1}));

    delete window;

    QCOMPARE(exitCodes, QList<int>({1}));
}

void TestGuiSmokeShutdown::synchronousDestructionExitsSuccess()
{
    QObject *window = new QObject;
    QList<int> exitCodes;

    QVERIFY(GuiSmokeShutdown::complete(
        window,
        [&window]() {
            delete window;
            window = nullptr;
            return true;
        },
        [&exitCodes](int code) { exitCodes.append(code); },
        0,
        1));
    QCOMPARE(exitCodes, QList<int>({0}));
}

void TestGuiSmokeShutdown::invalidInputsFailWithoutDuplicateExit()
{
    QList<int> exitCodes;

    QVERIFY(!GuiSmokeShutdown::complete(
        nullptr,
        []() { return true; },
        [&exitCodes](int code) { exitCodes.append(code); },
        0,
        2));
    QCOMPARE(exitCodes, QList<int>({2}));

    QObject *window = new QObject;
    QVERIFY(!GuiSmokeShutdown::complete(
        window,
        {},
        [&exitCodes](int code) { exitCodes.append(code); },
        0,
        3));
    delete window;
    QCOMPARE(exitCodes, QList<int>({2, 3}));

    QVERIFY(!GuiSmokeShutdown::complete(
        nullptr,
        {},
        {},
        0,
        4));
    QCOMPARE(exitCodes, QList<int>({2, 3}));
}

QTEST_GUILESS_MAIN(TestGuiSmokeShutdown)

#include "testGuiSmokeShutdown.moc"
