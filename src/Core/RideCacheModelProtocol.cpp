/*
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "RideCacheModel.h"

#include <QPointer>

quint64 RideCacheModel::reserveCacheMutation()
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->activeMutationReservation != 0
        || !state->frames.isEmpty()) {
        return 0;
    }
    if (++state->nextMutationReservation == 0)
        ++state->nextMutationReservation;
    state->activeMutationReservation =
        state->nextMutationReservation;
    return state->activeMutationReservation;
}

void RideCacheModel::releaseCacheMutation(quint64 reservation)
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (reservation == 0
        || state->activeMutationReservation != reservation) {
        return;
    }
    state->activeMutationReservation = 0;
    if (state->frames.isEmpty()) scheduleDeferredConfigChange();
}

bool RideCacheModel::beginReset()
{
    return beginResetImpl(0);
}

bool RideCacheModel::beginReservedReset(quint64 reservation)
{
    if (reservation == 0) return false;
    return beginResetImpl(reservation);
}

bool RideCacheModel::beginResetImpl(quint64 reservation)
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if ((state->activeMutationReservation != 0
         && state->activeMutationReservation != reservation)
        || (reservation != 0
            && state->activeMutationReservation != reservation)) {
        return false;
    }
    const bool coveredByReset = !state->frames.isEmpty()
        && state->frames.constFirst().protocol
            == ModelChangeState::Protocol::Reset;
    if (!state->frames.isEmpty() && !coveredByReset) return false;

    state->frames.append({
        ModelChangeState::Protocol::Reset,
        !coveredByReset});
    if (coveredByReset) return true;

    const QPointer<RideCacheModel> guardedThis(this);
    rideCache->invalidateStartupSnapshots();
    beginResetModel();
    if (!guardedThis) {
        state->frames.removeLast();
        return false;
    }
    return true;
}

void RideCacheModel::endReset()
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->frames.isEmpty()
        || state->frames.constLast().protocol
            != ModelChangeState::Protocol::Reset) {
        return;
    }

    const bool ownsQtProtocol = state->frames.constLast().ownsQtProtocol;
    if (!ownsQtProtocol) {
        state->frames.removeLast();
        return;
    }

    const QPointer<RideCacheModel> guardedThis(this);
    endResetModel();
    state->frames.removeLast();
    if (guardedThis && state->frames.isEmpty())
        guardedThis->scheduleDeferredConfigChange();
}

bool RideCacheModel::startInsert(int first, int last)
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->activeMutationReservation != 0) return false;
    const bool coveredByReset = !state->frames.isEmpty()
        && state->frames.constFirst().protocol
            == ModelChangeState::Protocol::Reset;
    if (!state->frames.isEmpty() && !coveredByReset) return false;

    state->frames.append({
        ModelChangeState::Protocol::Insert,
        !coveredByReset});
    if (coveredByReset) return true;

    beginInsertRows(QModelIndex(), first, last);
    return true;
}

void RideCacheModel::endInsert()
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->frames.isEmpty()
        || state->frames.constLast().protocol
            != ModelChangeState::Protocol::Insert) {
        return;
    }

    const bool ownsQtProtocol = state->frames.constLast().ownsQtProtocol;
    if (!ownsQtProtocol) {
        state->frames.removeLast();
        return;
    }

    const QPointer<RideCacheModel> guardedThis(this);
    endInsertRows();
    state->frames.removeLast();
    if (guardedThis && state->frames.isEmpty())
        guardedThis->scheduleDeferredConfigChange();
}

bool RideCacheModel::startRemove(int row)
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->activeMutationReservation != 0) return false;
    const bool coveredByReset = !state->frames.isEmpty()
        && state->frames.constFirst().protocol
            == ModelChangeState::Protocol::Reset;
    if (!state->frames.isEmpty() && !coveredByReset) return false;

    state->frames.append({
        ModelChangeState::Protocol::Remove,
        !coveredByReset});
    if (coveredByReset) return true;

    rideCache->invalidateStartupSnapshots();
    beginRemoveRows(QModelIndex(), row, row);
    return true;
}

void RideCacheModel::endRemove(int)
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->frames.isEmpty()
        || state->frames.constLast().protocol
            != ModelChangeState::Protocol::Remove) {
        return;
    }

    const bool ownsQtProtocol = state->frames.constLast().ownsQtProtocol;
    if (!ownsQtProtocol) {
        state->frames.removeLast();
        return;
    }

    const QPointer<RideCacheModel> guardedThis(this);
    endRemoveRows();
    state->frames.removeLast();
    if (guardedThis && state->frames.isEmpty())
        guardedThis->scheduleDeferredConfigChange();
}

bool RideCacheModel::cacheMutationAllowed() const
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    return state->activeMutationReservation == 0
        && (state->frames.isEmpty()
        || state->frames.constFirst().protocol
            == ModelChangeState::Protocol::Reset);
}

void RideCacheModel::scheduleDeferredConfigChange()
{
    const std::shared_ptr<ModelChangeState> state = modelChangeState_;
    if (state->deferredConfigScheduled
        || !state->deferredConfigPending) {
        return;
    }

    state->deferredConfigScheduled = true;
    const QPointer<RideCacheModel> guardedThis(this);
    const bool queued = QMetaObject::invokeMethod(
        this,
        [guardedThis, state] {
            state->deferredConfigScheduled = false;
            if (!guardedThis || !state->frames.isEmpty()
                || state->activeMutationReservation != 0) {
                return;
            }

            const qint32 changes = state->deferredConfigChanges;
            state->deferredConfigPending = false;
            state->deferredConfigChanges = 0;
            guardedThis->configChanged(changes);
        },
        Qt::QueuedConnection);
    if (!queued) state->deferredConfigScheduled = false;
}
