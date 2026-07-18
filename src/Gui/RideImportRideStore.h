/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEIMPORTRIDESTORE_H
#define GC_RIDEIMPORTRIDESTORE_H

#include <QStringList>
#include <QVector>

#include <utility>

template<typename Ride>
class BasicRideImportRideStore
{
public:
    BasicRideImportRideStore() = default;

    ~BasicRideImportRideStore()
    {
        clear();
    }

    BasicRideImportRideStore(const BasicRideImportRideStore &) = delete;
    BasicRideImportRideStore &operator=(
        const BasicRideImportRideStore &) = delete;

    void appendPending()
    {
        entries_.append(Entry());
    }

    void insertParsed(
        qsizetype index, Ride *ride,
        const QStringList &errors = QStringList())
    {
        Q_ASSERT(index >= 0 && index <= entries_.size());
        Entry entry;
        entry.ride = ride;
        entry.errors = errors;
        entry.parsed = true;
        entries_.insert(index, entry);
    }

    template<typename Parser>
    Ride *parseOnce(qsizetype index, Parser &&parser)
    {
        Entry &entry = entryAt(index);
        if (!entry.parsed) {
            entry.parsed = true;
            entry.errors.clear();
            entry.ride =
                std::forward<Parser>(parser)(entry.errors);
        }
        return entry.ride;
    }

    Ride *at(qsizetype index) const
    {
        return entryAt(index).ride;
    }

    Ride *take(qsizetype index)
    {
        Entry &entry = entryAt(index);
        Ride *ride = entry.ride;
        entry.ride = nullptr;
        return ride;
    }

    Ride *takeAndRemove(qsizetype index)
    {
        Ride *ride = take(index);
        entries_.removeAt(index);
        return ride;
    }

    void removeAt(qsizetype index)
    {
        delete take(index);
        entries_.removeAt(index);
    }

    const QStringList &errorsAt(qsizetype index) const
    {
        return entryAt(index).errors;
    }

    bool wasParsed(qsizetype index) const
    {
        return entryAt(index).parsed;
    }

    qsizetype size() const
    {
        return entries_.size();
    }

    void clear()
    {
        for (Entry &entry : entries_) {
            delete entry.ride;
            entry.ride = nullptr;
        }
        entries_.clear();
    }

private:
    struct Entry
    {
        Ride *ride = nullptr;
        QStringList errors;
        bool parsed = false;
    };

    Entry &entryAt(qsizetype index)
    {
        Q_ASSERT(index >= 0 && index < entries_.size());
        return entries_[index];
    }

    const Entry &entryAt(qsizetype index) const
    {
        Q_ASSERT(index >= 0 && index < entries_.size());
        return entries_[index];
    }

    QVector<Entry> entries_;
};

class RideFile;
using RideImportRideStore = BasicRideImportRideStore<RideFile>;

#endif // GC_RIDEIMPORTRIDESTORE_H
