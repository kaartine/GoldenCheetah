/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingControllerLifecycle_h
#define _GC_TrainingControllerLifecycle_h

namespace TrainingControllerLifecycle
{

template<typename Controller>
void stopAndDelete(Controller *&controller)
{
    Controller *ownedController = controller;
    controller = nullptr;
    if (!ownedController) return;

    ownedController->stop();
    delete ownedController;
}

}

#endif
