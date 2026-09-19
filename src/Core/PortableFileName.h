/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_PORTABLEFILENAME_H
#define GC_PORTABLEFILENAME_H

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace PortableFileName {

inline bool isValid(const QString &component)
{
    if (component.isEmpty()
        || component == QStringLiteral(".")
        || component == QStringLiteral("..")
        || QDir::isAbsolutePath(component)
        || QFileInfo(component).fileName() != component
        || component.endsWith(QLatin1Char(' '))
        || component.endsWith(QLatin1Char('.'))
        || component.toUtf8().size() > 240) {
        return false;
    }

    static const QString forbidden =
        QStringLiteral("<>:\"/\\|?*");
    for (const QChar character : component) {
        const ushort value = character.unicode();
        if (value <= 0x1f || value == 0x7f
            || forbidden.contains(character)) {
            return false;
        }
    }

    const int dot = component.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? component : component.left(dot))
        .toUpper();
    static const QStringList reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"),
        QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")};
    return !reserved.contains(stem);
}

} // namespace PortableFileName

#endif
