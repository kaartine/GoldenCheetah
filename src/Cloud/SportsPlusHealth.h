/*
 * Copyright (c) 2017 Mark Liversedge (liversedge@gmail.com)
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

#ifndef GC_SportsPlusHealth_h
#define GC_SportsPlusHealth_h

#include <QImage>

#include "CloudService.h"

class SportsPlusHealth : public CloudService
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("SportPlusHealth"); }
    QString uiName() const override { return tr("SportPlusHealth"); }
    QString description() const override
        { return tr("This discontinued service is disabled."); }
    QImage logo() const override
        { return QImage(":images/services/sportplushealth.png"); }

    explicit SportsPlusHealth(Context *context);
    CloudService *clone(Context *context) override
        { return new SportsPlusHealth(context); }
    ~SportsPlusHealth() override = default;

    int capabilities() const override { return 0; }

    bool open(QStringList &errors) override;
    bool close() override;
    bool writeFile(QByteArray &data, QString remotename,
                   RideFile *ride) override;
};

#endif
