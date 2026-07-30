/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CacheWriteWarning.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMessageBox>
#include <QPointer>
#include <QTest>
#include <QWidget>

class TestCacheWriteWarning : public QObject
{
    Q_OBJECT

private slots:
    void warningIsNonModalAndDeletesOnClose();
};

void TestCacheWriteWarning::warningIsNonModalAndDeletesOnClose()
{
    QWidget owner;
    const QString message =
        QStringLiteral("Cannot create cache file activity.cpx.");
    QPointer<QMessageBox> warning =
        CacheWriteWarning::show(&owner, message);

    QVERIFY(warning);
    QVERIFY(warning->isVisible());
    QCOMPARE(warning->parentWidget(), &owner);
    QCOMPARE(warning->text(), message);
    QCOMPARE(warning->icon(), QMessageBox::Warning);
    QCOMPARE(warning->standardButtons(), QMessageBox::Ok);
    QCOMPARE(warning->windowModality(), Qt::NonModal);
    QVERIFY(!warning->isModal());
    QVERIFY(warning->testAttribute(Qt::WA_DeleteOnClose));

    warning->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(warning.isNull());
}

QTEST_MAIN(TestCacheWriteWarning)

#include "testCacheWriteWarning.moc"
