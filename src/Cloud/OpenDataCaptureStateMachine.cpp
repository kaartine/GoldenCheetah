/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataCaptureStateMachine.h"

#include <utility>

namespace OpenDataCaptureStateMachine {

StateMachine::StateMachine(Operations operations)
    : operations_(std::move(operations))
{
}

AdvanceResult StateMachine::advance(QString &error)
{
    error.clear();
    if (phase_ == Phase::Complete)
        return AdvanceResult::Complete;
    if (phase_ == Phase::Cancelled)
        return AdvanceResult::Cancelled;
    if (phase_ == Phase::Failed) {
        error = failure_;
        return AdvanceResult::Failed;
    }
    if (cancellationRequested_ || !allowed()) {
        phase_ = Phase::Cancelled;
        return AdvanceResult::Cancelled;
    }

    try {
        switch (phase_) {
        case Phase::Startup:
            if (!operations_.startupReady)
                return fail(
                    error,
                    QStringLiteral(
                        "OpenData startup readiness is unavailable"));
            if (!operations_.startupReady())
                return AdvanceResult::Waiting;
            phase_ = Phase::Capture;
            return AdvanceResult::More;

        case Phase::Capture:
            if (!operations_.captureSnapshot
                || !operations_.captureSnapshot(
                    sourceCount_, error)
                || sourceCount_ < 0) {
                return fail(
                    error,
                    QStringLiteral(
                        "Cannot capture the OpenData snapshot"));
            }
            phase_ = sourceCount_ > 0
                ? Phase::Sources
                : Phase::Validate;
            return AdvanceResult::More;

        case Phase::Sources:
            if (!operations_.processSource
                || !operations_.processSource(
                    sourceIndex_, error)) {
                return fail(
                    error,
                    QStringLiteral(
                        "Cannot process an OpenData activity"));
            }
            ++sourceIndex_;
            if (sourceIndex_ >= sourceCount_)
                phase_ = Phase::Validate;
            return AdvanceResult::More;

        case Phase::Validate:
            if (!operations_.validateSnapshot
                || !operations_.validateSnapshot(error)) {
                return fail(
                    error,
                    QStringLiteral(
                        "The OpenData snapshot changed during capture"));
            }
            phase_ = Phase::Seal;
            return AdvanceResult::More;

        case Phase::Seal:
            if (!operations_.sealArchive
                || !operations_.sealArchive(error)) {
                return fail(
                    error,
                    QStringLiteral(
                        "Cannot seal the OpenData archive"));
            }
            phase_ = Phase::Describe;
            return AdvanceResult::More;

        case Phase::Describe: {
            if (!operations_.describeArchive)
                return fail(
                    error,
                    QStringLiteral(
                        "Cannot describe the OpenData archive"));
            const DescriptionResult result =
                operations_.describeArchive(error);
            if (result == DescriptionResult::Invalid) {
                return fail(
                    error,
                    QStringLiteral(
                        "Cannot describe the OpenData archive"));
            }
            if (result == DescriptionResult::Complete)
                phase_ = Phase::Handoff;
            return AdvanceResult::More;
        }

        case Phase::Handoff:
            if (!operations_.handoff)
                return fail(
                    error,
                    QStringLiteral(
                        "OpenData upload handoff is unavailable"));
            operations_.handoff();
            phase_ = Phase::Complete;
            return AdvanceResult::Complete;

        case Phase::Complete:
            return AdvanceResult::Complete;
        case Phase::Cancelled:
            return AdvanceResult::Cancelled;
        case Phase::Failed:
            error = failure_;
            return AdvanceResult::Failed;
        }
    } catch (...) {
        return fail(
            error,
            QStringLiteral(
                "Unexpected OpenData capture failure"));
    }

    return fail(
        error,
        QStringLiteral("Invalid OpenData capture state"));
}

void StateMachine::requestCancellation()
{
    if (phase_ == Phase::Complete
        || phase_ == Phase::Failed) {
        return;
    }
    cancellationRequested_ = true;
}

AdvanceResult StateMachine::fail(
    QString &error,
    const QString &fallback)
{
    if (error.isEmpty()) error = fallback;
    failure_ = error;
    phase_ = Phase::Failed;
    return AdvanceResult::Failed;
}

bool StateMachine::allowed() const
{
    if (!operations_.allowed) return false;
    try {
        return operations_.allowed();
    } catch (...) {
        return false;
    }
}

} // namespace OpenDataCaptureStateMachine
