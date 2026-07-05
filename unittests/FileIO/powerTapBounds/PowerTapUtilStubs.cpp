/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "PowerTapUtil.h"

bool PowerTapUtil::is_Ver81(unsigned char *)
{
    return false;
}

int PowerTapUtil::is_time(unsigned char *, bool)
{
    return false;
}

time_t PowerTapUtil::unpack_time(unsigned char *, struct tm *, bool)
{
    return 0;
}

int PowerTapUtil::is_config(unsigned char *, bool)
{
    return false;
}

int PowerTapUtil::unpack_config(unsigned char *, unsigned *, unsigned *,
                                double *, unsigned *, bool)
{
    return 0;
}
