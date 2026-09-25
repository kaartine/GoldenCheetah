/*
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

#ifndef _GC_SharedStyle_h
#define _GC_SharedStyle_h 1

#include <QApplication>
#include <QPointer>
#include <QStyle>
#include <QStyleFactory>

#include "Settings.h"

// QWidget::setStyle() does not take ownership, and one style is often set on
// several widgets, so no single widget can own it. All such widgets share one
// Fusion instance owned by the application.
inline QStyle *sharedFusionStyle()
{
    static QPointer<QStyle> style;
    if (!style) {
        style = QStyleFactory::create(OS_STYLE);
        if (style) style->setParent(qApp);
    }
    return style.data();
}

#endif
