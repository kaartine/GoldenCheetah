/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutFileWriter_h
#define _GC_WorkoutFileWriter_h

#include <QSaveFile>
#include <QString>
#include <QTextStream>

#include <memory>

class WorkoutFileWriter
{
public:
    explicit WorkoutFileWriter(const QString &path);

    bool open(QString &error);
    QTextStream &stream();
    bool commit(QString &error);

    static QString ensureSuffix(const QString &path, const QString &suffix);

private:
    QSaveFile file;
    std::unique_ptr<QTextStream> output;
};

#endif // _GC_WorkoutFileWriter_h
