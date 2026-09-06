/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWorkoutIdentity.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

QString workoutGameWorkoutIdentity(const QString &fileName)
{
    const QString path = QFileInfo(fileName).absoluteFilePath();
    if (path.isEmpty()) return QString();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return path;

    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(64 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            return path;
        }
        digest.addData(block);
    }
    return path + QLatin1Char('\n')
            + QString::fromLatin1(digest.result().toHex());
}
