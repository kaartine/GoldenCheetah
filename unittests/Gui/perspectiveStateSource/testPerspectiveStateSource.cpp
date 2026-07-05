/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Gui/PerspectiveStateSource.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestPerspectiveStateSource : public QObject
{
    Q_OBJECT

private slots:

    void resetUsesBundledStateInsteadOfSavedState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString savedFileName = directory.filePath("train-perspectives.xml");
        QFile savedFile(savedFileName);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        const QByteArray untrustedState(
            "<layouts><layout><chart id=\"45\"><property name=\"script\" "
            "type=\"QString\" value=\"untrusted-code\"/></chart></layout></layouts>");
        QCOMPARE(savedFile.write(untrustedState), untrustedState.size());
        savedFile.close();

        const QByteArray state =
            PerspectiveStateSource::load(savedFileName, "train", true);

        QVERIFY(state.contains("Trusted bundled default"));
        QVERIFY(!state.contains("untrusted-code"));
    }

    void normalRestoreUsesSavedState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString savedFileName = directory.filePath("train-perspectives.xml");
        QFile savedFile(savedFileName);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        const QByteArray savedState("<layouts><layout name=\"Saved\"/></layouts>");
        QCOMPARE(savedFile.write(savedState), savedState.size());
        savedFile.close();

        QCOMPARE(PerspectiveStateSource::load(savedFileName, "train", false),
                 savedState);
    }

    void resetRejectsUnknownViewNames()
    {
        QVERIFY(PerspectiveStateSource::load(QString(), "../application", true).isEmpty());
        QVERIFY(PerspectiveStateSource::load(QString(), "unknown", true).isEmpty());
    }
};

QTEST_MAIN(TestPerspectiveStateSource)
#include "testPerspectiveStateSource.moc"
