/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_CacheWriteWarning_h
#define _GC_CacheWriteWarning_h

class QMessageBox;
class QString;
class QWidget;

namespace CacheWriteWarning {

QMessageBox *show(
    QWidget *owner,
    const QString &message);

} // namespace CacheWriteWarning

#endif // _GC_CacheWriteWarning_h
