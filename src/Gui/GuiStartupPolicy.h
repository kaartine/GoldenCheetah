/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_GUI_STARTUP_POLICY_H
#define GC_GUI_STARTUP_POLICY_H

#include <QString>

namespace GuiStartupPolicy {

bool shouldPrimeWebEngine(const QString &platformName);

}

#endif
