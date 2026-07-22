/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef TESTJSONIMPORTINTEGRITY_H
#define TESTJSONIMPORTINTEGRITY_H

#include <QObject>

class TestJsonImportIntegrity : public QObject
{
    Q_OBJECT

private slots:
    void importsCompleteDocument();
    void acceptsValidDocumentWithExistingDiagnostics();
    void rejectsMalformedDocuments_data();
    void rejectsMalformedDocuments();
};

#endif
