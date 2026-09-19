/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_RDEFERREDUIWORK_H
#define GC_RDEFERREDUIWORK_H

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <utility>

class RDeferredUiWork final
{
public:
    struct Batch
    {
        QList<QPointer<QObject>> chartReruns;
        QList<QPointer<QObject>> consolePrompts;

        bool isEmpty() const
        {
            return chartReruns.isEmpty() && consolePrompts.isEmpty();
        }
    };

    bool requestChartRerun(QObject *target)
    {
        return appendUnique(chartReruns_, target);
    }

    bool requestConsolePrompt(QObject *target)
    {
        return appendUnique(consolePrompts_, target);
    }

    Batch take()
    {
        return {
            std::exchange(chartReruns_, {}),
            std::exchange(consolePrompts_, {})};
    }

    static void post(Batch batch)
    {
        // Preserve this order: a chart rerun may write to its console, while
        // a prompt must remain the final deferred UI update.
        for (const QPointer<QObject> &target : batch.chartReruns) {
            if (target) {
                QMetaObject::invokeMethod(
                    target.data(), "runScript", Qt::QueuedConnection);
            }
        }
        for (const QPointer<QObject> &target : batch.consolePrompts) {
            if (target) {
                QMetaObject::invokeMethod(
                    target.data(), "ensurePrompt", Qt::QueuedConnection);
            }
        }
    }

private:
    static bool appendUnique(
        QList<QPointer<QObject>> &targets,
        QObject *target)
    {
        if (!target) return false;
        for (const QPointer<QObject> &existing : targets) {
            if (existing.data() == target) return true;
        }
        targets.append(QPointer<QObject>(target));
        return true;
    }

    QList<QPointer<QObject>> chartReruns_;
    QList<QPointer<QObject>> consolePrompts_;
};

#endif
