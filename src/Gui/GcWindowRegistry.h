/*
 * Copyright (c) 2010 Mark Liversedge (liversedge@gmail.com)
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

#ifndef _GC_GcWindowRegistry_h
#define _GC_GcWindowRegistry_h

#include "GcWindowTypes.h"
#include "GoldenCheetah.h"
#include <QApplication>

class Context;

// when declaring a window, what view is it relevant for?
#define VIEW_TRAIN    0x01
#define VIEW_ANALYSIS 0x02
#define VIEW_PLAN 0x04
#define VIEW_TRENDS   0x08

class GcChartWindow;
class GcWindowRegistry {
    Q_DECLARE_TR_FUNCTIONS(GcWindowRegistry)
    public:

    unsigned int relevance;
    QString name;
    GcWinID id;

    static void initialize(); // initialize global registry
    static GcChartWindow *newGcWindow(GcWinID id, Context *context);
    static QStringList windowsForType(int type);
    static QList<GcWinID> idsForType(int type);
    static QString title(GcWinID id);
    static unsigned int relevanceForId(GcWinID id);
};

extern GcWindowRegistry* GcWindows;
#endif
