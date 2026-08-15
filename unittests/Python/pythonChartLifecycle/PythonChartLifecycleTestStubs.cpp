/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Specification.h"

#include <utility>

DateRange::DateRange(QDate from, QDate to, QString name, QColor color)
    : from(from), to(to), name(std::move(name)), color(color),
      valid(from.isValid() && to.isValid())
{
}

DateRange::DateRange(const DateRange &other)
    : from(other.from), to(other.to), name(other.name), color(other.color),
      id(other.id), valid(from.isValid() && to.isValid())
{
}

DateRange &DateRange::operator=(const DateRange &other)
{
    from = other.from;
    to = other.to;
    name = other.name;
    color = other.color;
    id = other.id;
    valid = from.isValid() && to.isValid();
    return *this;
}

PlanFilter::PlanFilter(PlanFilterType type)
    : type(type)
{
}

Specification::Specification()
    : it(nullptr), recintsecs(0), ri(nullptr)
{
}
