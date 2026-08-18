/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutFileWriter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestWorkoutFileWriter : public QObject
{
    Q_OBJECT

private slots:
    void writesAndAtomicallyReplacesWorkout()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("ride.erg"));

        QString error;
        WorkoutFileWriter first(path);
        QVERIFY2(first.open(error), qPrintable(error));
        first.stream() << "first\n";
        QVERIFY2(first.commit(error), qPrintable(error));

        WorkoutFileWriter replacement(path);
        QVERIFY2(replacement.open(error), qPrintable(error));
        replacement.stream() << "replacement\n";
        QVERIFY2(replacement.commit(error), qPrintable(error));

        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("replacement\n"));
    }

    void rejectsMissingParentDirectoryWithoutLeavingAFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(
                QStringLiteral("missing/ride.erg"));

        QString error;
        WorkoutFileWriter writer(path);
        QVERIFY(!writer.open(error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(path));
    }

    void normalizesSuffixCaseInsensitively()
    {
        QCOMPARE(WorkoutFileWriter::ensureSuffix("ride", ".erg"),
                 QString("ride.erg"));
        QCOMPARE(WorkoutFileWriter::ensureSuffix("ride.ERG", ".erg"),
                 QString("ride.ERG"));
        QCOMPARE(WorkoutFileWriter::ensureSuffix("ride", "mrc"),
                 QString("ride.mrc"));
    }
};

QTEST_APPLESS_MAIN(TestWorkoutFileWriter)

#include "testWorkoutFileWriter.moc"
