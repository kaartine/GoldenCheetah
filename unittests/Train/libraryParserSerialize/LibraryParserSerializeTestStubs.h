/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_LIBRARY_PARSER_SERIALIZE_TEST_STUBS_H
#define GC_LIBRARY_PARSER_SERIALIZE_TEST_STUBS_H

#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>

class Library
{
public:
    QString name;
    QStringList paths;
    QStringList refs;
};

extern QList<Library *> libraries;

class LibraryParser
{
public:
    static bool serialize(QDir home, QString *error = nullptr);
};

#endif // GC_LIBRARY_PARSER_SERIALIZE_TEST_STUBS_H
