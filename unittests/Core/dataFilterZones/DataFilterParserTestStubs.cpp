/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Core/DataFilter.h"

QStringList DataFiltererrors;
Leaf *DataFilterroot = nullptr;

namespace Utils {

QString unescape(const QString &value)
{
    return value;
}

} // namespace Utils

void Leaf::clear(Leaf *leaf)
{
    if (!leaf) return;

    switch (leaf->type) {
    case Leaf::Script:
    case Leaf::String:
        delete leaf->lvalue.s;
        break;
    case Leaf::Symbol:
        delete leaf->lvalue.n;
        break;
    case Leaf::Logical:
    case Leaf::BinaryOperation:
    case Leaf::Operation:
        clear(leaf->lvalue.l);
        clear(leaf->rvalue.l);
        delete leaf->lvalue.l;
        delete leaf->rvalue.l;
        break;
    case Leaf::UnaryOperation:
        clear(leaf->lvalue.l);
        delete leaf->lvalue.l;
        break;
    case Leaf::Function:
        clear(leaf->lvalue.l);
        delete leaf->lvalue.l;
        clear(leaf->series);
        delete leaf->series;
        for (Leaf *parameter : leaf->fparms) {
            clear(parameter);
            delete parameter;
        }
        leaf->fparms.clear();
        break;
    case Leaf::Compound:
        for (Leaf *statement : *leaf->lvalue.b) {
            clear(statement);
            delete statement;
        }
        delete leaf->lvalue.b;
        break;
    case Leaf::Conditional:
        clear(leaf->lvalue.l);
        clear(leaf->rvalue.l);
        clear(leaf->cond.l);
        delete leaf->lvalue.l;
        delete leaf->rvalue.l;
        delete leaf->cond.l;
        break;
    case Leaf::Index:
    case Leaf::Select:
        clear(leaf->lvalue.l);
        delete leaf->lvalue.l;
        for (Leaf *parameter : leaf->fparms) {
            clear(parameter);
            delete parameter;
        }
        leaf->fparms.clear();
        break;
    default:
        break;
    }
}
