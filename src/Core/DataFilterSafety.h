/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_DataFilterSafety_h
#define GC_DataFilterSafety_h

#include <QString>
#include <QtGlobal>

namespace DataFilterSafety {

struct EstimateValues {
    double cp = 0.0;
    double wPrime = 0.0;
    double ftp = 0.0;
    double pMax = 0.0;
    double dateFrom = 0.0;
    double dateTo = 0.0;
};

struct EstimatePair {
    bool valid = false;
    double first = 0.0;
    double second = 0.0;
};

EstimatePair estimatePair(const QString &parameter,
                          bool durationRequested,
                          bool durationModelMatched,
                          double durationValue,
                          const EstimateValues &values);
qsizetype repeatedVectorIndex(qsizetype valueCount,
                              qsizetype assignmentOffset);
bool vectorAssignmentIndex(double value, int *index);

} // namespace DataFilterSafety

#endif
