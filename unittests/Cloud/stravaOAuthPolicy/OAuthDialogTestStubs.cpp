/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Cloud/CloudService.h"

#include <QSslError>

double dpiXFactor = 1.0;
double dpiYFactor = 1.0;

void CloudService::sslErrors(
    QWidget *, QNetworkReply *, QList<QSslError>)
{
}
