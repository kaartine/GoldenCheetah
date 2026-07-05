/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PerspectiveStateSource.h"

#include <QFile>
#include <QStringList>

QByteArray
PerspectiveStateSource::load(const QString &savedFileName,
                             const QString &view,
                             bool useDefault)
{
    QString fileName = savedFileName;

    if (useDefault) {
        static const QStringList bundledViews = {
            QStringLiteral("analysis"),
            QStringLiteral("home"),
            QStringLiteral("plan"),
            QStringLiteral("train")
        };

        if (!bundledViews.contains(view)) return QByteArray();
        fileName = QStringLiteral(":/xml/%1-perspectives.xml").arg(view);
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.readAll();
}
