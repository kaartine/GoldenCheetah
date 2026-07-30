/*
 * Copyright (c) 2018 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef GC_OpenData_h
#define GC_OpenData_h

#include "Context.h"

#include <QDialog>
#include <QScrollArea>
#include <QPushButton>

class OpenData
{
public:
    static void check(Context *);
};

class OpenDataDialog : public QDialog
{
    Q_OBJECT

public:
    OpenDataDialog(Context *);

private slots:
    void acceptConditions();
    void rejectConditions();

private:

    Context *context;

    QScrollArea *scrollText;
    QPushButton *proceedButton;
    QPushButton *abortButton;

};

#endif
