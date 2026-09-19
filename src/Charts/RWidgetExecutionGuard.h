/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RWIDGETEXECUTIONGUARD_H
#define GC_RWIDGETEXECUTIONGUARD_H

#include <QPointer>

#include <initializer_list>
#include <utility>
#include <vector>

class RWidgetExecutionGuard
{
public:
    RWidgetExecutionGuard(
        QObject *owner,
        std::initializer_list<QObject *> dependencies = {})
        : owner_(owner)
    {
        dependencies_.reserve(dependencies.size());
        for (QObject *dependency : dependencies) dependencies_.emplace_back(dependency);
    }

    bool isValid() const
    {
        if (owner_.isNull()) return false;
        for (const QPointer<QObject> &dependency : dependencies_) {
            if (dependency.isNull()) return false;
        }
        return true;
    }

    template <typename Evaluation>
    bool runAndValidate(Evaluation &&evaluation)
    {
        std::forward<Evaluation>(evaluation)();
        return isValid();
    }

private:
    QPointer<QObject> owner_;
    std::vector<QPointer<QObject>> dependencies_;
};

#endif
