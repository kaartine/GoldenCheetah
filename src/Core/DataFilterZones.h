/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_DataFilterZones_h
#define GC_DataFilterZones_h

#include "DataFilterSafety.h"

class Leaf;

namespace DataFilterZones {

DataFilterSafety::ZoneArguments arguments(const Leaf *leaf);
bool validate(Leaf *leaf);
int validateTree(Leaf *leaf);

} // namespace DataFilterZones

#endif
