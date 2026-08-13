/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_TRAININGSESSION_H
#define GC_TRAININGSESSION_H

#include <QString>

#include <memory>

class ErgFile;
class HtmlTrainingBridge;
class TrainingApplicationService;
class VideoSyncFile;

class TrainingSession final
{
public:
    explicit TrainingSession(
        std::unique_ptr<TrainingApplicationService> applicationService);
    ~TrainingSession();

    TrainingSession(const TrainingSession &) = delete;
    TrainingSession &operator=(const TrainingSession &) = delete;

    HtmlTrainingBridge *htmlTrainingBridge();
    ErgFile *currentWorkout() const;
    void setWorkout(ErgFile *workout);
    VideoSyncFile *currentVideoSync() const;
    void setVideoSync(VideoSyncFile *videoSync);
    const QString &mediaFilename() const;
    void setMediaFilename(const QString &filename);
    long now() const;
    void setNow(long now);
    bool isRunning() const;
    bool isPaused() const;
    void setStatus(bool running, bool paused);

private:
    std::unique_ptr<TrainingApplicationService> applicationService_;
    ErgFile *workout_ = nullptr;
    VideoSyncFile *videoSync_ = nullptr;
    QString mediaFilename_;
    long now_ = 0;
    bool running_ = false;
    bool paused_ = false;
};

#endif // GC_TRAININGSESSION_H
