/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>

#include "Library.h"
#include "Settings.h"

class TestLibraryOwnership : public QObject
{
    Q_OBJECT

    static QPointer<QObject> observe(Library *library)
    {
        // Library privately inherits QObject. Its generated meta-object cast
        // exposes the real QObject for a destruction probe without changing
        // production inheritance or substituting a test implementation.
        return static_cast<QObject *>(library->qt_metacast("QObject"));
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(!qEnvironmentVariableIsEmpty("GC_CHART_OWNERSHIP_ROOT"),
                 "Use the isolated chartOwnership launcher");
        const QString root = qEnvironmentVariable("GC_CHART_OWNERSHIP_ROOT");
        QCOMPARE(qEnvironmentVariable("XDG_CONFIG_HOME"), root + "/config");
        QVERIFY(appsettings);
        appsettings->setValue(GC_WORKOUTDIR, QString());
        QVERIFY(libraries.isEmpty());
    }

    void cleanup()
    {
        Library::releaseAll();
        QVERIFY(libraries.isEmpty());
    }

    void releaseEmptyRegistryIsIdempotent()
    {
        Library::releaseAll();
        Library::releaseAll();
        QVERIFY(libraries.isEmpty());
        QVERIFY(!Library::findLibrary("Media Library"));
    }

    void repeatedInitializationRetainsSharedLibrary()
    {
        QTemporaryDir firstRoot;
        QTemporaryDir secondRoot;
        QVERIFY(firstRoot.isValid());
        QVERIFY(secondRoot.isValid());
        Library::initialise(QDir(firstRoot.path()));
        QCOMPARE(libraries.size(), 1);
        Library *library = Library::findLibrary("Media Library");
        QVERIFY(library);
        const QPointer<QObject> owner = observe(library);
        QVERIFY(owner);
        const QList<QString> firstPaths = library->paths;
        QVERIFY(!firstPaths.isEmpty());
        library->refs.append("retained-reference.mp4");

        // New windows and in-process restarts call the same initialization.
        Library::initialise(QDir(secondRoot.path()));
        QCOMPARE(libraries.size(), 1);
        QVERIFY(Library::findLibrary("Media Library") == library);
        QCOMPARE(library->paths, firstPaths);
        QCOMPARE(library->refs, QList<QString>({"retained-reference.mp4"}));

        Library::releaseAll();
        QVERIFY(owner.isNull());
        QVERIFY(libraries.isEmpty());
        QVERIFY(!Library::findLibrary("Media Library"));
        Library::releaseAll();
    }

    void initializationAfterReleaseStartsFresh()
    {
        QTemporaryDir firstRoot;
        QTemporaryDir secondRoot;
        QVERIFY(firstRoot.isValid());
        QVERIFY(secondRoot.isValid());
        Library::initialise(QDir(firstRoot.path()));
        Library *library = Library::findLibrary("Media Library");
        QVERIFY(library);
        const QPointer<QObject> oldOwner = observe(library);
        QVERIFY(oldOwner);
        library->refs.append("old-reference.mp4");
        Library::releaseAll();
        QVERIFY(oldOwner.isNull());

        Library::initialise(QDir(secondRoot.path()));
        QCOMPARE(libraries.size(), 1);
        library = Library::findLibrary("Media Library");
        QVERIFY(library);
        QVERIFY(!library->paths.isEmpty());
        QVERIFY(library->refs.isEmpty());
        const QPointer<QObject> newOwner = observe(library);
        QVERIFY(newOwner);
        Library::releaseAll();
        QVERIFY(newOwner.isNull());
    }

    void releaseDeletesEveryParsedLibrary()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QFile xml(root.filePath("library.xml"));
        QVERIFY(xml.open(QIODevice::WriteOnly));
        const QByteArray contents =
            "<libraries>"
            "<library name=\"Media Library\"><path>/media</path>"
            "<ref>ride.mp4</ref></library>"
            "<library name=\"Other Library\"><path>/other</path></library>"
            "</libraries>";
        QCOMPARE(xml.write(contents), qint64(contents.size()));
        xml.close();

        Library::initialise(QDir(root.path()));
        QCOMPARE(libraries.size(), 2);
        Library *media = Library::findLibrary("Media Library");
        Library *other = Library::findLibrary("Other Library");
        QVERIFY(media);
        QVERIFY(other);
        QCOMPARE(media->refs, QList<QString>({"ride.mp4"}));
        const QPointer<QObject> mediaOwner = observe(media);
        const QPointer<QObject> otherOwner = observe(other);
        QVERIFY(mediaOwner);
        QVERIFY(otherOwner);

        Library::releaseAll();
        QVERIFY(mediaOwner.isNull());
        QVERIFY(otherOwner.isNull());
        QVERIFY(libraries.isEmpty());
        QVERIFY(!Library::findLibrary("Other Library"));
        Library::releaseAll();
    }
};

QTEST_MAIN(TestLibraryOwnership)
#include "testLibraryOwnership.moc"
