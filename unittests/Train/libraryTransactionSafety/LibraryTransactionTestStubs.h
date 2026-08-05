#ifndef GC_LIBRARY_TRANSACTION_TEST_STUBS_H
#define GC_LIBRARY_TRANSACTION_TEST_STUBS_H

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QMessageBox>
#include <QStringList>
#include <QVariant>

#include "ErgFileBase.h"
#include "VideoSyncFileBase.h"

enum class LibraryBatchImportConfirmation {
    forcedDialog,
    optionalDialog,
    noDialog
};

struct LibraryImportResult
{
    bool completed = false;
    QStringList requestedFiles;
    QHash<QString, QString> importedVideos;
    QHash<QString, QString> importedWorkouts;
    QHash<QString, QString> importedVideoSyncs;
    QStringList failedFiles;

    bool allSucceeded() const
    {
        if (!completed || !failedFiles.isEmpty()) {
            return false;
        }
        for (const QString &file : requestedFiles) {
            if (!contains(importedVideos, file)
                && !contains(importedWorkouts, file)
                && !contains(importedVideoSyncs, file)) {
                return false;
            }
        }
        return true;
    }

private:
    static bool contains(const QHash<QString, QString> &available,
                         const QString &source)
    {
        return available.contains(source) && !available.value(source).isEmpty();
    }
};

class TestAthleteHome
{
public:
    explicit TestAthleteHome(const QDir &directory) : directory(directory) {}
    QDir root() const { return directory; }

private:
    QDir directory;
};

struct Athlete
{
    TestAthleteHome *home = nullptr;
};

struct Context
{
    Athlete *athlete = nullptr;
    QString selectedVideo;
    QString selectedWorkout;
    QString selectedVideoSync;

    void notifySelectVideo(const QString &path) { selectedVideo = path; }
    void notifySelectWorkout(const QString &path) { selectedWorkout = path; }
    void notifySelectVideoSync(const QString &path) { selectedVideoSync = path; }
};

class Library
{
public:
    QString name;
    QList<QString> refs;

    static QString tr(const char *text) { return QString::fromUtf8(text); }
    static Library *findLibrary(QString name);
    static LibraryImportResult importFiles(
        Context *context,
        QStringList files,
        LibraryBatchImportConfirmation confirmation =
            LibraryBatchImportConfirmation::optionalDialog);
    static bool refreshWorkouts(Context *context);
};

extern QList<Library *> libraries;

class TestAppSettings
{
public:
    QString workoutDirectory;

    QVariant value(void *, const QString &) const
    {
        return workoutDirectory;
    }
};

extern TestAppSettings *appsettings;

#define GC_WORKOUTDIR QStringLiteral("workout-directory")

class LibraryParser
{
public:
    static bool serialize(const QDir &) { return true; }
};

class MediaHelper
{
public:
    bool isMedia(const QString &path) const
    {
        return QFileInfo(path).suffix().compare(
                   QStringLiteral("mp4"), Qt::CaseInsensitive) == 0;
    }
};

class ErgFile : public ErgFileBase
{
public:
    ErgFile(const QString &path, ErgFileFormat, Context *);

    static bool isWorkout(const QString &path);
    bool isValid() const { return valid; }

private:
    bool valid = false;
};

class VideoSyncFile : public VideoSyncFileBase
{
public:
    VideoSyncFile(const QString &path, int &, Context *);

    static bool isVideoSync(const QString &path);
    bool isValid() const { return valid; }

private:
    bool valid = false;
};

class WorkoutImportDialog
{
public:
    WorkoutImportDialog(Context *, const QStringList &) {}
    int exec() { return 0; }
};

#endif // GC_LIBRARY_TRANSACTION_TEST_STUBS_H
