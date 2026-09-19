/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_CLOUDDBCHARTIMPORTPOLICY_H
#define GC_CLOUDDBCHARTIMPORTPOLICY_H

#include <QList>
#include <QMap>
#include <QString>

namespace CloudDBChartImportPolicy {

enum class Decision {
    Allow,
    RejectMalformed,
    RejectExecutableType,
    RejectExecutableProperty
};

Decision evaluate(
    const QList<QMap<QString, QString>> &chartProperties);

} // namespace CloudDBChartImportPolicy

#endif
