/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_CompressedActivityFile_h
#define _GC_CompressedActivityFile_h 1

#include <QIODevice>

#include <memory>

namespace CompressedActivityFile {

enum class Format {
    Zip,
    Gzip
};

bool extractSingleFile(
    std::unique_ptr<QIODevice> source,
    Format format,
    QIODevice *destination);

} // namespace CompressedActivityFile

#endif // _GC_CompressedActivityFile_h
