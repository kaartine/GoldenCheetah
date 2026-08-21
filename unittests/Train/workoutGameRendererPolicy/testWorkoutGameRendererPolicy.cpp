/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameRendererPolicy.h"

#include <QTest>

class TestWorkoutGameRendererPolicy : public QObject
{
    Q_OBJECT

private slots:
    void normalDesktopUsesSceneGraph()
    {
        const WorkoutGameRendererDecision decision =
                WorkoutGameRendererPolicy::decide(false, "xcb", 4.6);
        QCOMPARE(decision.backend, WorkoutGameRendererBackend::SceneGraph);
        QCOMPARE(decision.reason,
                 WorkoutGameRendererSelectionReason::Preferred);
        QCOMPARE(QString::fromLatin1(WorkoutGameRendererPolicy::backendName(
                         decision.backend)), QStringLiteral("SceneGraph"));
    }

    void explicitPainterOverrideWins()
    {
        QCOMPARE(WorkoutGameRendererPolicy::choose(true, "xcb", 4.6),
                 WorkoutGameRendererBackend::Painter);
        QCOMPARE(WorkoutGameRendererPolicy::decide(
                         true, "xcb", 4.6).reason,
                 WorkoutGameRendererSelectionReason::ForcedPainter);
    }

    void widgetlessPlatformsUsePainter_data()
    {
        QTest::addColumn<QString>("platform");
        QTest::newRow("offscreen") << QStringLiteral("offscreen");
        QTest::newRow("minimal") << QStringLiteral("minimal");
        QTest::newRow("case-insensitive") << QStringLiteral("OFFSCREEN");
    }

    void widgetlessPlatformsUsePainter()
    {
        QFETCH(QString, platform);
        QCOMPARE(WorkoutGameRendererPolicy::choose(
                         false, platform.toStdString(), 4.6),
                 WorkoutGameRendererBackend::Painter);
    }

    void insufficientOpenGLUsesPainter()
    {
        QCOMPARE(WorkoutGameRendererPolicy::choose(false, "xcb", 1.5),
                 WorkoutGameRendererBackend::Painter);
        QCOMPARE(WorkoutGameRendererPolicy::decide(
                         false, "xcb", 1.5).reason,
                 WorkoutGameRendererSelectionReason::InsufficientOpenGL);
    }

    void sceneGraphFailureUsesLegacyOpenGL()
    {
        QCOMPARE(WorkoutGameRendererPolicy::afterInitializationFailure(
                         WorkoutGameRendererBackend::SceneGraph),
                 WorkoutGameRendererBackend::OpenGL);
    }

    void legacyOpenGLFailureUsesPainter()
    {
        QCOMPARE(WorkoutGameRendererPolicy::afterInitializationFailure(
                         WorkoutGameRendererBackend::OpenGL),
                 WorkoutGameRendererBackend::Painter);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameRendererPolicy)
#include "testWorkoutGameRendererPolicy.moc"
