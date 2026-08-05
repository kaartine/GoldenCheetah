/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "TrainingSession.h"

#include "SessionServices.h"

#include <QtGlobal>

TrainingSession::TrainingSession(
    std::unique_ptr<TrainingApplicationService> applicationService)
    : applicationService_(std::move(applicationService))
{
    Q_ASSERT(applicationService_);
}

TrainingSession::~TrainingSession() = default;

HtmlTrainingBridge *TrainingSession::htmlTrainingBridge()
{
    return applicationService_->htmlTrainingBridge();
}

ErgFile *TrainingSession::currentWorkout() const
{
    return workout_;
}

void TrainingSession::setWorkout(ErgFile *workout)
{
    workout_ = workout;
}

VideoSyncFile *TrainingSession::currentVideoSync() const
{
    return videoSync_;
}

void TrainingSession::setVideoSync(VideoSyncFile *videoSync)
{
    videoSync_ = videoSync;
}

const QString &TrainingSession::mediaFilename() const
{
    return mediaFilename_;
}

void TrainingSession::setMediaFilename(const QString &filename)
{
    mediaFilename_ = filename;
}

long TrainingSession::now() const
{
    return now_;
}

void TrainingSession::setNow(long now)
{
    now_ = now;
}

bool TrainingSession::isRunning() const
{
    return running_;
}

bool TrainingSession::isPaused() const
{
    return paused_;
}

void TrainingSession::setStatus(bool running, bool paused)
{
    running_ = running;
    paused_ = paused;
}
