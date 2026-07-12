/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "DataFilterSafety.h"

#include <climits>
#include <cmath>

namespace DataFilterSafety {

EstimatePair estimatePair(const QString &parameter,
                          bool durationRequested,
                          bool durationModelMatched,
                          double durationValue,
                          const EstimateValues &values)
{
    if (durationRequested) {
        if (!durationModelMatched) {
            return {};
        }
        return {true, durationValue, durationValue};
    }

    if (parameter == QStringLiteral("cp")) {
        return {true, values.cp, values.cp};
    }
    if (parameter == QStringLiteral("w'")) {
        return {true, values.wPrime, values.wPrime};
    }
    if (parameter == QStringLiteral("ftp")) {
        return {true, values.ftp, values.ftp};
    }
    if (parameter == QStringLiteral("pmax")) {
        return {true, values.pMax, values.pMax};
    }
    if (parameter == QStringLiteral("date")) {
        return {true, values.dateFrom, values.dateTo};
    }

    return {};
}

qsizetype repeatedVectorIndex(qsizetype valueCount,
                              qsizetype assignmentOffset)
{
    if (valueCount <= 0 || assignmentOffset < 0) {
        return -1;
    }
    return assignmentOffset % valueCount;
}

bool vectorAssignmentIndex(double value, int *index)
{
    if (!index
        || !std::isfinite(value)
        || value < 0.0
        || value > static_cast<double>(INT_MAX - 1)) {
        return false;
    }

    *index = static_cast<int>(value);
    return true;
}

} // namespace DataFilterSafety
