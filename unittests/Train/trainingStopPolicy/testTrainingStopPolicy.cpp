/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/TrainingStopPolicy.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestTrainingStopPolicy : public QObject
{
    Q_OBJECT

private slots:
    void deviceErrorPreservesRawRecording()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("partial.csv"));
        const QByteArray samples("secs,watts\n0,180\n1,205\n");

        QFile recording(path);
        QVERIFY(recording.open(QIODevice::WriteOnly));
        QCOMPARE(recording.write(samples), samples.size());
        recording.close();

        const TrainingStopPolicy::RecordingAction action =
                TrainingStopPolicy::controllerStopAction(true);
        QCOMPARE(action, TrainingStopPolicy::RecordingAction::Keep);
        QVERIFY(TrainingStopPolicy::applyFileAction(recording, action));

        QVERIFY(recording.open(QIODevice::ReadOnly));
        QCOMPARE(recording.readAll(), samples);
    }

    void normalControllerStopImportsRecording()
    {
        QCOMPARE(TrainingStopPolicy::controllerStopAction(false),
                 TrainingStopPolicy::RecordingAction::Import);
    }

    void onlyExplicitDiscardRemovesRecording()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("discard.csv"));

        QFile recording(path);
        QVERIFY(recording.open(QIODevice::WriteOnly));
        QCOMPARE(recording.write("sample\n"), qint64(7));
        recording.close();

        QVERIFY(TrainingStopPolicy::applyFileAction(
                recording, TrainingStopPolicy::RecordingAction::Discard));
        QVERIFY(!recording.exists());
        QVERIFY(TrainingStopPolicy::controllerStopAction(true) !=
                TrainingStopPolicy::RecordingAction::Discard);
        QVERIFY(TrainingStopPolicy::controllerStopAction(false) !=
                TrainingStopPolicy::RecordingAction::Discard);
    }
};

QTEST_MAIN(TestTrainingStopPolicy)
#include "testTrainingStopPolicy.moc"
