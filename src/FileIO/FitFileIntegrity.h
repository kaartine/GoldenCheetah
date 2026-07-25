/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_FitFileIntegrity_h
#define GC_FitFileIntegrity_h

#include <QIODevice>
#include <QString>

namespace FitFileIntegrity {

inline constexpr qint64 MaximumFileSize =
    512LL * 1024 * 1024;

struct ValidationResult {
    bool valid = false;
    QString error;
    int segmentCount = 0;
};

ValidationResult validate(
    QIODevice &device,
    qint64 maximumFileSize = MaximumFileSize);

bool consumeRecordBytes(qint64 &remaining,
                        qint64 consumed);

} // namespace FitFileIntegrity

#endif
