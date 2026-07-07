/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef gc_LibUsb_h
#define gc_LibUsb_h

#define TYPE_ANT 0

namespace FakeAntTransport
{

struct Snapshot
{
    int liveInstances = 0;
    int openCount = 0;
    int closeCount = 0;
    int blockedReaders = 0;
    bool leased = false;
};

void reset();
void setReadDelay(int delayMs);
bool waitForBlockedReader(int timeoutMs = 1000);
Snapshot snapshot();

}

class LibUsb
{
public:
    explicit LibUsb(int type);
    ~LibUsb();

    int open();
    void close();
    int read(char *buffer, int bytes);
    int read(char *buffer, int bytes, int timeout);
    int write(char *buffer, int bytes);
    int write(char *buffer, int bytes, int timeout);
    bool find();

private:
    bool opened = false;
};

#endif
