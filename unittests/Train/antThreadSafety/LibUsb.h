#ifndef GC_ANT_THREAD_SAFETY_LIB_USB_H
#define GC_ANT_THREAD_SAFETY_LIB_USB_H

#include <QByteArray>
#include <QtGlobal>
#include <QVector>

#define TYPE_ANT 0

namespace FakeAntTransport
{

struct WriteCall
{
    QByteArray bytes;
    quintptr threadId = 0;
};

struct Snapshot
{
    int liveInstances = 0;
    int openCount = 0;
    int closeCount = 0;
    int blockedReaders = 0;
    int messageWrites = 0;
    bool leased = false;
    bool firstMessageBlocked = false;
    bool interleaveTimedOut = false;
    QVector<WriteCall> writes;
};

void reset();

void enableProducerReadBarrier();
bool synchronizeProducerWithRead(int timeoutMs = 5000);
void disableProducerReadBarrier();

void armMessageInterleave();
bool waitForFirstMessageBlocked(int timeoutMs = 2000);
bool waitForMessageWrites(int count, int timeoutMs = 10000);
void queueReadBytes(const QByteArray &bytes);

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
