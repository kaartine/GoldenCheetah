/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHTARGETREGISTRY_H
#define GC_RIDEREFRESHTARGETREGISTRY_H

#include "RideRefreshItemInputs.h"

#include <QHash>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <limits>

inline quint64 reserveRideRefreshIdentity(
    std::atomic<quint64> &nextIdentity)
{
    quint64 candidate = nextIdentity.load(std::memory_order_relaxed);
    while (candidate != 0) {
        const quint64 following = candidate
                == std::numeric_limits<quint64>::max()
            ? 0 : candidate + 1;
        if (nextIdentity.compare_exchange_weak(
                candidate, following,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0;
}

template<typename Target>
class RideRefreshTargetRegistry final
{
public:
    RideRefreshTargetRegistry() = default;

    bool initialize(
        QThread *ownerThread,
        quint64 cacheEpoch,
        quint64 firstTargetId = 1,
        quint64 firstRevision = 1)
    {
        if (ownerThread_ || !ownerThread || cacheEpoch == 0
            || firstTargetId == 0 || firstRevision == 0) {
            return false;
        }
        ownerThread_ = ownerThread;
        cacheEpoch_ = cacheEpoch;
        nextTargetId_ = firstTargetId;
        firstRevision_ = firstRevision;
        return true;
    }

    RideRefreshTargetToken registerTarget(Target *target)
    {
        if (!onOwnerThread() || !target || cacheEpoch_ == 0) return {};
        pruneDestroyedTargets();

        const auto known = targetIds_.constFind(target);
        if (known != targetIds_.cend()) {
            const quint64 knownId = *known;
            const auto entry = targets_.constFind(*known);
            if (entry != targets_.cend()
                && entry->target.data() == target) {
                return tokenFor(*known, *entry);
            }
            targetIds_.erase(known);
            targets_.remove(knownId);
        }

        if (nextTargetId_ == 0) return {};
        const quint64 targetId = nextTargetId_;
        nextTargetId_ = targetId
                == std::numeric_limits<quint64>::max()
            ? 0 : targetId + 1;
        Entry entry;
        entry.address = target;
        entry.target = target;
        entry.revision = firstRevision_;
        targets_.insert(targetId, entry);
        targetIds_.insert(target, targetId);
        return tokenFor(targetId, entry);
    }

    RideRefreshTargetToken advanceRevision(Target *target)
    {
        if (!onOwnerThread() || !target) return {};
        const auto known = targetIds_.constFind(target);
        if (known == targetIds_.cend()) return {};
        auto entry = targets_.find(*known);
        if (entry == targets_.end()
            || entry->target.data() != target) {
            return {};
        }
        if (entry->revision
            == std::numeric_limits<quint64>::max()) {
            retire(target);
            return {};
        }
        ++entry->revision;
        return tokenFor(*known, *entry);
    }

    bool retire(Target *target)
    {
        if (!onOwnerThread() || !target) return false;
        const auto known = targetIds_.find(target);
        if (known == targetIds_.end()) return false;
        const quint64 targetId = *known;
        targetIds_.erase(known);
        targets_.remove(targetId);
        return true;
    }

    Target *resolve(const RideRefreshTargetToken &token) const
    {
        if (!onOwnerThread() || token.cacheEpoch != cacheEpoch_
            || !token.isValid()) {
            return nullptr;
        }
        const auto entry = targets_.constFind(token.targetId);
        if (entry == targets_.cend()
            || entry->revision != token.revision) {
            return nullptr;
        }
        return entry->target.data();
    }

    qsizetype entryCount()
    {
        if (!onOwnerThread()) return -1;
        pruneDestroyedTargets();
        return targets_.size();
    }

private:
    struct Entry {
        Target *address = nullptr;
        QPointer<Target> target;
        quint64 revision = 1;
    };

    void pruneDestroyedTargets()
    {
        for (auto entry = targets_.begin(); entry != targets_.end();) {
            if (entry->target) {
                ++entry;
                continue;
            }
            const quint64 targetId = entry.key();
            const auto reverse = targetIds_.find(entry->address);
            if (reverse != targetIds_.end() && *reverse == targetId) {
                targetIds_.erase(reverse);
            }
            entry = targets_.erase(entry);
        }
    }

    bool onOwnerThread() const
    {
        return ownerThread_
            && QThread::currentThread() == ownerThread_;
    }

    RideRefreshTargetToken tokenFor(
        quint64 targetId,
        const Entry &entry) const
    {
        return {cacheEpoch_, targetId, entry.revision};
    }

    QThread *ownerThread_ = nullptr;
    quint64 cacheEpoch_ = 0;
    quint64 nextTargetId_ = 0;
    quint64 firstRevision_ = 1;
    QHash<quint64, Entry> targets_;
    QHash<Target *, quint64> targetIds_;
};

#endif // GC_RIDEREFRESHTARGETREGISTRY_H
