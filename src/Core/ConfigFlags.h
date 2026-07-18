/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_CONFIGFLAGS_H
#define GC_CONFIGFLAGS_H

#define CONFIG_ATHLETE           0x1        // default weight, height, etc.
#define CONFIG_ZONES             0x2        // CP, FTP, and zone configuration
#define CONFIG_GENERAL           0x4        // weight, W'bal, and directories
#define CONFIG_USERMETRICS       0x8        // user-defined metrics
#define CONFIG_APPEARANCE        0x10
#define CONFIG_FIELDS            0x20       // metadata fields
#define CONFIG_NOTECOLOR         0x40       // activity coloring from metadata
#define CONFIG_METRICS           0x100      // interval/best/summary metrics
#define CONFIG_DEVICES           0x200
#define CONFIG_SEASONS           0x400      // seasons, events, and PMC seeds
#define CONFIG_UNITS             0x800      // metric or imperial units
#define CONFIG_PMC               0x1000     // PMC constants
#define CONFIG_WBAL              0x2000     // W'bal formula
#define CONFIG_WORKOUTS          0x4000     // workout locations and files
#define CONFIG_DISCOVERY         0x8000     // interval discovery
#define CONFIG_WORKOUTTAGMANAGER 0x10000    // workout tags

#endif // GC_CONFIGFLAGS_H
