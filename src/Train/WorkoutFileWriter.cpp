/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutFileWriter.h"

#include <QFileInfo>

WorkoutFileWriter::WorkoutFileWriter(const QString &path) : file(path)
{
    file.setDirectWriteFallback(false);
}

bool WorkoutFileWriter::open(QString &error)
{
    error.clear();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }
    output = std::make_unique<QTextStream>(&file);
    return true;
}

QTextStream &WorkoutFileWriter::stream()
{
    return *output;
}

bool WorkoutFileWriter::commit(QString &error)
{
    error.clear();
    if (!output) {
        error = QStringLiteral("Workout file is not open");
        return false;
    }

    output->flush();
    if (output->status() != QTextStream::Ok) {
        error = file.errorString();
        file.cancelWriting();
        output.reset();
        return false;
    }
    output.reset();

    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

QString WorkoutFileWriter::ensureSuffix(
        const QString &path,
        const QString &suffix)
{
    const QString normalized = suffix.startsWith(QLatin1Char('.'))
            ? suffix
            : QStringLiteral(".") + suffix;
    return path.endsWith(normalized, Qt::CaseInsensitive)
            ? path
            : path + normalized;
}
