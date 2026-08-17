/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_VirtualDrivetrain_h
#define _GC_VirtualDrivetrain_h

class VirtualDrivetrain
{
public:
    explicit VirtualDrivetrain(int initialGear = 6);

    int minimumGear() const;
    int maximumGear() const;
    int gear() const;
    int chainringTeeth() const;
    int sprocketTeeth() const;

    bool shiftUp();
    bool shiftDown();
    bool setGear(int gear);
    void reset();

    double gearRatio() const;
    double relativeRatio() const;
    double speedKph(double cadenceRpm, double wheelCircumferenceMeters) const;

private:
    static int clampGear(int gear);

    int initialGear_;
    int gear_;
};

#endif
