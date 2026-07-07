/*
 * Copyright (c) 2012 Mark Liversedge (liversedge@gmail.com)
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

#ifndef _Library_h
#define _Library_h
#include "GoldenCheetah.h"
#include "ActionButtonBox.h"

#include <QDir>
#include <QHash>
#include <QLabel>
#include <QStringList>
#include <QDialog>
#include <QFileDialog>
#include <QCheckBox>
#include <QPushButton>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QThread>


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

    bool importedAll(const QStringList &expectedWorkouts,
                     const QStringList &expectedVideos,
                     const QStringList &expectedVideoSyncs) const
    {
        QStringList expectedFiles =
            expectedWorkouts + expectedVideos + expectedVideoSyncs;
        QStringList actualFiles = requestedFiles;
        expectedFiles.sort();
        actualFiles.sort();
        return completed
            && failedFiles.isEmpty()
            && actualFiles == expectedFiles
            && containsAll(importedWorkouts, expectedWorkouts)
            && containsAll(importedVideos, expectedVideos)
            && containsAll(importedVideoSyncs, expectedVideoSyncs);
    }

private:
    static bool containsAll(const QHash<QString, QString> &available,
                            const QStringList &expected)
    {
        for (const QString &file : expected) {
            if (!contains(available, file)) {
                return false;
            }
        }
        return true;
    }

    static bool contains(const QHash<QString, QString> &available,
                         const QString &source)
    {
        return available.contains(source) && !available.value(source).isEmpty();
    }
};

class Library : QObject
{
    Q_OBJECT

	public:
        QString name;           // e.g. Media Library
        QList<QString> paths;   // array of search paths for files in this library
        QList<QString> refs;    // array of drag-n-dropped files referenced not copied

        static void initialise(QDir gcRoot); // init
        static Library *findLibrary(QString);
        static LibraryImportResult importFiles(Context *context, QStringList files, LibraryBatchImportConfirmation dialog=LibraryBatchImportConfirmation::optionalDialog);
        void removeRef(Context *context, QString ref);

        static bool refreshWorkouts(Context *context);
};

extern QList<Library *> libraries;        // keep track of all Library search paths for all users

class LibrarySearch;
class LibrarySearchDialog : public QDialog
{
    Q_OBJECT

    public:
        LibrarySearchDialog(Context *context);

    private slots:
        void search();
        void cancel();

        void pathsearching(QString);
        void foundWorkout(QString);
        void foundVideo(QString);
        void foundVideoSync(QString);

        void addDirectory();
        void removeDirectory();
        void removeReference();
        void updateDB();

    private:
        Context *context;
        Library *library;
        LibrarySearch *searcher;
        bool searching;
        int pathIndex, workoutCountN, videoCountN, videosyncCountN;

        QStringList workoutsFound, videosFound, videosyncsFound;

        // let us know we are searching
        void setSearching(bool amsearching) {
            searching = amsearching;
            setWidgets();
        }
        
        // update widgets to switch between searching and not searching
        void setWidgets();

        // gui widgets
        QCheckBox *findWorkouts,
                  *findVideoSyncs,
                  *findMedia;
        ActionButtonBox *actionButtons;
        QPushButton *removeRef;
        QTreeWidget *searchPathTable;
        QTreeWidget *refTable;
        QTreeWidgetItem *allRefs;
        QTreeWidgetItem *allPaths;
        QLabel *pathLabelTitle, *mediaCountTitle, *videosyncCountTitle, *workoutCountTitle;
        QLabel *pathLabel, *mediaCount, *videosyncCount, *workoutCount;
        QPushButton *cancelButton,
                    *searchButton;
};

class LibrarySearch : public QThread
{
    Q_OBJECT

    public:
        LibrarySearch(QString path, bool findMedia, bool findVideoSync, bool findWorkout);
        void run();

    public slots:
        void abort();


    signals:
        void searching(QString);
        void done();
        void foundVideo(QString);
        void foundVideoSync(QString);
        void foundWorkout(QString);

    private:
        volatile bool aborted;
        QString path;
        bool findMedia, findWorkout, findVideoSync;
};

class WorkoutImportDialog : public QDialog
{
    Q_OBJECT

    public:
        WorkoutImportDialog(Context *context, QStringList files);

    public slots:
        void import();

    private:
        Context *context;
        QStringList files;
 
        QStringList videos, videosyncs, workouts;

        QTreeWidget *fileTable;
        QPushButton *okButton, *cancelButton;

        QCheckBox *overwrite;
};

#endif // _Library_h
