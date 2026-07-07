#ifndef GC_ANT_THREAD_SAFETY_SETTINGS_H
#define GC_ANT_THREAD_SAFETY_SETTINGS_H

#include <QObject>
#include <QString>
#include <QVariant>

class TestSettings
{
public:
    QVariant value(const QObject *owner, const QString &key,
                   const QVariant &fallback = QVariant());
    void setValue(const QString &key, const QVariant &value);
    QVariant cvalue(const QString &athlete, const QString &key,
                    const QVariant &fallback = QVariant());
};

extern TestSettings *appsettings;
extern int OperatingSystem;

#define WINDOWS 1
#define LINUX 2
#define OSX 3
#define OPENBSD 4

#define GC_WHEELSIZE "<athlete-preferences>wheelsize"
#define SRM_OFFSET "<global-trainmode>srm/offset"

#endif
