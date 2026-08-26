/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAudio.h"

#include <QSoundEffect>
#include <QUrl>

class WorkoutGameAudioFeedback::Effects
{
public:
    Effects()
    {
        feature.setSource(QUrl(QStringLiteral(
                "qrc:/audio/workout-game-feature.wav")));
        feature.setLoopCount(1);
        feature.setVolume(0.18);
        landing.setSource(QUrl(QStringLiteral(
                "qrc:/audio/workout-game-landing.wav")));
        landing.setLoopCount(1);
        landing.setVolume(0.24);
    }

    QSoundEffect feature;
    QSoundEffect landing;
};

WorkoutGameAudioFeedback::WorkoutGameAudioFeedback() = default;

WorkoutGameAudioFeedback::~WorkoutGameAudioFeedback() = default;

void WorkoutGameAudioFeedback::setEnabled(bool newEnabled)
{
    enabled = newEnabled;
    if (enabled && !effects) effects = std::make_unique<Effects>();
    if (!enabled && effects) {
        effects->feature.stop();
        effects->landing.stop();
    }
}

void WorkoutGameAudioFeedback::reset()
{
    tracker.reset();
    if (effects) {
        effects->feature.stop();
        effects->landing.stop();
    }
}

void WorkoutGameAudioFeedback::update(
        const WorkoutGameAudioEventJournal &journal)
{
    const WorkoutGameAudioEvents events = tracker.update(journal);
    if (!enabled || !effects) return;
    if (events.feature) effects->feature.play();
    if (events.landing) {
        effects->landing.setVolume(
                0.16 + 0.20 * events.landingStrength);
        effects->landing.play();
    }
}
