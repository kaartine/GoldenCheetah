/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "DataFilterZones.h"

#include "DataFilter.h"

namespace DataFilterZones {

DataFilterSafety::ZoneArguments arguments(const Leaf *leaf)
{
    const Leaf *series = nullptr;
    const Leaf *field = nullptr;
    if (!leaf
        || leaf->type != Leaf::Function
        || leaf->function != QStringLiteral("zones")
        || leaf->fparms.size() != 2) {
        return {};
    }

    series = leaf->fparms.at(0);
    field = leaf->fparms.at(1);
    if (!series
        || !field
        || series->type != Leaf::Symbol
        || field->type != Leaf::Symbol
        || !series->lvalue.n
        || !field->lvalue.n) {
        return {};
    }

    return DataFilterSafety::zoneArguments(
        *series->lvalue.n,
        *field->lvalue.n);
}

bool validate(Leaf *leaf)
{
    const DataFilterSafety::ZoneArguments normalized = arguments(leaf);
    if (!normalized.valid) {
        if (leaf) leaf->inerror = true;
        return false;
    }

    *leaf->fparms.at(0)->lvalue.n = normalized.series;
    *leaf->fparms.at(1)->lvalue.n = normalized.field;
    leaf->inerror = false;
    return true;
}

int validateTree(Leaf *leaf)
{
    if (!leaf) return 0;

    int newlyInvalid = 0;
    if (leaf->type == Leaf::Function
        && leaf->function == QStringLiteral("zones")) {
        const bool alreadyInvalid = leaf->inerror;
        if (!validate(leaf) && !alreadyInvalid) {
            ++newlyInvalid;
        }
    }

    const auto validateChild = [&newlyInvalid](Leaf *child) {
        newlyInvalid += validateTree(child);
    };

    switch (leaf->type) {
    case Leaf::Logical:
    case Leaf::BinaryOperation:
    case Leaf::Operation:
        validateChild(leaf->lvalue.l);
        validateChild(leaf->rvalue.l);
        break;
    case Leaf::UnaryOperation:
        validateChild(leaf->lvalue.l);
        break;
    case Leaf::Function:
        validateChild(leaf->lvalue.l);
        validateChild(leaf->series);
        for (Leaf *parameter : leaf->fparms) {
            validateChild(parameter);
        }
        break;
    case Leaf::Compound:
        if (leaf->lvalue.b) {
            for (Leaf *statement : *leaf->lvalue.b) {
                validateChild(statement);
            }
        }
        break;
    case Leaf::Conditional:
        validateChild(leaf->lvalue.l);
        validateChild(leaf->rvalue.l);
        validateChild(leaf->cond.l);
        break;
    case Leaf::Index:
    case Leaf::Select:
        validateChild(leaf->lvalue.l);
        for (Leaf *parameter : leaf->fparms) {
            validateChild(parameter);
        }
        break;
    default:
        break;
    }

    return newlyInvalid;
}

} // namespace DataFilterZones
