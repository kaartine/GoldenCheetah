/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHEBULKMERGE_H
#define GC_RIDECACHEBULKMERGE_H

#include <QHash>
#include <QVector>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace RideCacheBulkMerge {

template<
    typename Item, typename KeyFunction, typename LessFunction,
    typename BeginResetFunction, typename EndResetFunction>
QVector<Item *> mergeItems(
    QVector<Item *> &current,
    const QVector<Item *> &incoming,
    KeyFunction keyFor,
    LessFunction lessThan,
    BeginResetFunction beginReset,
    EndResetFunction endReset)
{
    QVector<Item *> replaced;
    if (incoming.isEmpty()) return replaced;

    using Key = std::decay_t<decltype(
        keyFor(std::declval<const Item *>()))>;
    QHash<Key, qsizetype> positions;
    positions.reserve(current.size() + incoming.size());

    for (qsizetype index = 0; index < current.size(); ++index) {
        Q_ASSERT(current[index]);
        positions.insert(keyFor(current[index]), index);
    }

    beginReset();
    for (Item *item : incoming) {
        Q_ASSERT(item);
        const Key key = keyFor(item);
        const auto existing = positions.constFind(key);
        if (existing != positions.cend()) {
            const qsizetype index = existing.value();
            replaced.append(current[index]);
            current[index] = item;
        } else {
            positions.insert(key, current.size());
            current.append(item);
        }
    }
    std::sort(current.begin(), current.end(), lessThan);
    endReset();

    return replaced;
}

} // namespace RideCacheBulkMerge

#endif // GC_RIDECACHEBULKMERGE_H
