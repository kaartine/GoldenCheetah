/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OpenDataCaptureStateMachine_h
#define GC_OpenDataCaptureStateMachine_h

#include <QString>
#include <QtTypes>

#include <functional>

namespace OpenDataCaptureStateMachine {

enum class DescriptionResult
{
    InProgress,
    Complete,
    Invalid
};

enum class AdvanceResult
{
    Waiting,
    More,
    Complete,
    Cancelled,
    Failed
};

struct Operations
{
    std::function<bool()> allowed;
    std::function<bool()> startupReady;
    std::function<bool(qsizetype &, QString &)> captureSnapshot;
    std::function<bool(qsizetype, QString &)> processSource;
    std::function<bool(QString &)> validateSnapshot;
    std::function<bool(QString &)> sealArchive;
    std::function<DescriptionResult(QString &)> describeArchive;
    std::function<void()> handoff;
};

class StateMachine final
{
public:
    explicit StateMachine(Operations operations);

    AdvanceResult advance(QString &error);
    void requestCancellation();

private:
    enum class Phase
    {
        Startup,
        Capture,
        Sources,
        Validate,
        Seal,
        Describe,
        Handoff,
        Complete,
        Cancelled,
        Failed
    };

    AdvanceResult fail(
        QString &error,
        const QString &fallback);
    bool allowed() const;

    Operations operations_;
    Phase phase_ = Phase::Startup;
    qsizetype sourceCount_ = 0;
    qsizetype sourceIndex_ = 0;
    QString failure_;
    bool cancellationRequested_ = false;
};

} // namespace OpenDataCaptureStateMachine

#endif
