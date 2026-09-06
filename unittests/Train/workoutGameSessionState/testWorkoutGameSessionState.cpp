/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameSessionState.h"
#include "Train/WorkoutGameWorkoutIdentity.h"

#include <QFile>
#include <QTemporaryDir>
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

    void ignoresLateWorkoutSelectionDuringAnActiveSession()
    {
        WorkoutGameSessionState state;
        QVERIFY(state.acceptsWorkoutSelection());
        QVERIFY(!state.hasDeferredWorkoutSelection());

        state.started();
        QVERIFY(!state.acceptsWorkoutSelection());
        state.workoutSelectionDeferred();
        QVERIFY(state.hasDeferredWorkoutSelection());

        state.stopped();
        QVERIFY(state.acceptsWorkoutSelection());
        QVERIFY(state.hasDeferredWorkoutSelection());

        state.workoutSelected();
        QVERIFY(!state.hasDeferredWorkoutSelection());
    }

    void explicitSeekIsConsumedExactlyOnce()
    {
        WorkoutGameSessionState state;
        QVERIFY(!state.consumePositionDiscontinuity());

        state.positionDiscontinuityRequested();
        QVERIFY(state.consumePositionDiscontinuity());
        QVERIFY(!state.consumePositionDiscontinuity());

        state.positionDiscontinuityRequested();
        state.workoutSelected();
        QVERIFY(!state.consumePositionDiscontinuity());
    }

    void workoutIdentityChangesWhenSameFileIsRewritten()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("course.crs"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("first course\n"), qint64(13));
        file.close();

        const QString first = workoutGameWorkoutIdentity(path);
        QVERIFY(!first.isEmpty());

        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("second route\n"), qint64(13));
        file.close();

        const QString second = workoutGameWorkoutIdentity(path);
        QVERIFY(!second.isEmpty());
        QVERIFY(first != second);
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameSessionState)
#include "testWorkoutGameSessionState.moc"
