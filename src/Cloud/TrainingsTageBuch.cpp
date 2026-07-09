/*
 * Copyright (c) 2017 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2013 Damien.Grauser (damien.grauser@pev-geneve.ch)
 * Copyright (c) 2012 Rainer Clasen <bj@zuto.de>
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

#include "TrainingsTageBuch.h"

TrainingsTageBuch::TrainingsTageBuch(Context *context)
    : CloudService(context)
{
    uploadCompression = none;
    filetype = CloudService::uploadType::PWX;
    useMetric = true;
}

bool
TrainingsTageBuch::open(QStringList &errors)
{
    errors << tr(
        "Trainingstagebuch has been discontinued and is disabled.");
    return false;
}

bool
TrainingsTageBuch::close()
{
    return true;
}

bool
TrainingsTageBuch::writeFile(QByteArray &data, QString remotename,
                             RideFile *ride)
{
    Q_UNUSED(data);
    Q_UNUSED(remotename);
    Q_UNUSED(ride);
    return false;
}
