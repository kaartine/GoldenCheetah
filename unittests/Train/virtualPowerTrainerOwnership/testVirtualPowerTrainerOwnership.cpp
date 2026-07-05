/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/RealtimeController.h"

#include <QTest>

#include <cstring>

class TestVirtualPowerTrainerOwnership : public QObject
{
    Q_OBJECT

private slots:
    void parsedNameOwnership();
    void directNameOwnershipRepeatedly();
};

void TestVirtualPowerTrainerOwnership::parsedNameOwnership()
{
    VirtualPowerTrainerManager manager;

    const int index = manager.PushCustomVirtualPowerTrainer(
        QStringLiteral("RPR,2,2|0,1|1|0|Parsed custom trainer"));

    QVERIFY(index > 0);
}

void TestVirtualPowerTrainerOwnership::directNameOwnershipRepeatedly()
{
    for (int iteration = 0; iteration < 64; ++iteration) {
        VirtualPowerTrainerManager manager;
        const QByteArray name = QByteArrayLiteral("Direct custom trainer ")
            + QByteArray::number(iteration);
        char *nameCopy = new char[static_cast<size_t>(name.size()) + 1];
        std::memcpy(nameCopy, name.constData(), static_cast<size_t>(name.size()) + 1);

        VirtualPowerTrainer *trainer = new VirtualPowerTrainer;
        trainer->m_pName = nameCopy;

        QCOMPARE(manager.PushCustomVirtualPowerTrainer(trainer), 1);
    }
}

QTEST_APPLESS_MAIN(TestVirtualPowerTrainerOwnership)
#include "testVirtualPowerTrainerOwnership.moc"
