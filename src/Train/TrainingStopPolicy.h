/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingStopPolicy_h
#define _GC_TrainingStopPolicy_h

#include <QFile>

namespace TrainingStopPolicy
{

enum class RecordingAction {
    Import,
    Keep,
    Discard
};

inline RecordingAction controllerStopAction(bool deviceError)
{
    return deviceError ? RecordingAction::Keep : RecordingAction::Import;
}

inline bool applyFileAction(QFile &recording, RecordingAction action)
{
    if (action != RecordingAction::Discard || !recording.exists()) return true;
    return recording.remove();
}

}

#endif
