/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameSessionState.h"

#include <QTest>

class TestWorkoutGameSessionState : public QObject
{
    Q_OBJECT

private slots:
    void acceptsPreviewAndRunningPositionUpdates()
    {
        WorkoutGameSessionState state;
        QVERIFY(state.acceptsPositionUpdate(false));

        state.started();
        QVERIFY(state.acceptsPositionUpdate(true));
        QVERIFY(!state.acceptsPositionUpdate(false));
    }

    void holdsTheFinalFrameAfterStop()
    {
        WorkoutGameSessionState state;
        state.started();
        state.stopped();

        QVERIFY(state.holdsStoppedFrame());
        QVERIFY(!state.acceptsPositionUpdate(false));
        QVERIFY(state.acceptsPositionUpdate(true));
    }

    void newWorkoutOrStartReleasesTheHeldFrame()
    {
        WorkoutGameSessionState state;
        state.stopped();
        state.workoutSelected();
        QVERIFY(!state.holdsStoppedFrame());
        QVERIFY(state.acceptsPositionUpdate(false));

        state.stopped();
        state.started();
        QVERIFY(!state.holdsStoppedFrame());
        QVERIFY(!state.acceptsPositionUpdate(false));
        QVERIFY(state.acceptsPositionUpdate(true));
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameSessionState)
#include "testWorkoutGameSessionState.moc"
