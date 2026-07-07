#ifndef GC_ANT_THREAD_SAFETY_DEVICE_CONFIGURATION_H
#define GC_ANT_THREAD_SAFETY_DEVICE_CONFIGURATION_H

#include <QString>

class RealtimeController;

class DeviceConfiguration
{
public:
    DeviceConfiguration();

    int type;
    QString name;
    QString portSpec;
    QString deviceProfile;
    int wheelSize;
    double inertialMomentKGM2;
    int stridelength;
    int postProcess;
    QString virtualPowerDefinitionString;
    RealtimeController *controller;
};

#endif
