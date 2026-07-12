/*
 * Copyright (c) 2006 Sean C. Rhea (srhea@srhea.net)
 * Copyright (c) 2015 Joern Rischmueller (joern.rm@gmail.com)
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

#include <QDir>
#include "Settings.h"
#include "CredentialSettings.h"
#include "MainWindow.h"
#include "Colors.h"
#include <QSettings>
#include <QDebug>
#include <QtAlgorithms>

#include <QFontDatabase>
#include <QCryptographicHash>

#ifdef Q_OS_MAC
int OperatingSystem = OSX;
#elif defined Q_OS_WIN32
int OperatingSystem = WINDOWS;
#elif defined Q_OS_LINUX
int OperatingSystem = LINUX;
#elif defined Q_OS_OPENBSD
int OperatingSystem = OPENBSD;
#endif

double scalefactors[13] = { 0.5f, 0.6f, 0.8, 0.9, 1.0f, 1.1f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 5.0f, 0 };

// -------------- Initializer for the "extern" variable "appsettings" ----------------//

static GSettings *GetApplicationSettings()
{
  GSettings *settings;
  QDir home = QDir();
    //First check to see if the Library folder exists where the executable is (for USB sticks)
  if(!home.exists("Library/GoldenCheetah"))
    settings = new GSettings(GC_SETTINGS_CO, GC_SETTINGS_APP);
  else
    settings = new GSettings(home.absolutePath()+"/gc", QSettings::IniFormat);
  return settings;
}

// local static helper routines

// define the sections and the filenames
enum SettingsType {SETTINGS_SYSTEM = 0,
                   SETTINGS_GLOBAL = 1,
                   SETTINGS_ATHLETE = 2 };

enum SettingsFilesIndexGlobal {GLOBAL_GENERAL = 0,
                               GLOBAL_TRAINMODE = 1};

enum SettingsFilesIndexAthlete { ATHLETE_GENERAL = 0,
                                 ATHLETE_LAYOUT = 1,
                                 ATHLETE_PREFERENCES = 2,
                                 ATHLETE_PRIVATE = 3};

static const QString settingFileNamesGlobal[2] = {"configglobal-general.ini","configglobal-trainmode.ini"};
static const QString settingFileNamesAthlete[4] = {"athlete-general.ini","athlete-layout.ini","athlete-preferences.ini","athlete-private.ini"};
static const QString credentialScopeStorageKey =
    QStringLiteral("credential_store/id");

static QString legacyCredentialScopeStorageKey(
    const QString &athleteName)
{
    const QByteArray identity = athleteName.isEmpty()
        ? QByteArray("global")
        : QByteArray("athlete:") + athleteName.toUtf8();
    const QByteArray digest = QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("credential_store/scopes/")
        + QString::fromLatin1(digest);
}

static void mirrorCredentialScope(
    QSettings *settings,
    const QString &storageKey,
    const QString &scopeId)
{
    if (!settings || storageKey.isEmpty() || scopeId.isEmpty()
        || settings->value(storageKey).toString() == scopeId) {
        return;
    }

    settings->setValue(storageKey, scopeId);
    settings->sync();
    CredentialSettings::hardenSettingsFile(settings);
    if (settings->status() != QSettings::NoError) {
        qWarning() << "Cannot persist credential scope mapping:"
                   << settings->fileName();
    }
}


static QString DetermineKey(QString & key, int& store, int& fileIndex) {

    store = SETTINGS_SYSTEM; // default to systemsettings
    fileIndex = 0;
    if (key.startsWith(GC_QSETTINGS_GLOBAL_GENERAL)) {
        store = SETTINGS_GLOBAL;
        fileIndex = GLOBAL_GENERAL;
    } else if (key.startsWith(GC_QSETTINGS_GLOBAL_TRAIN)) {
        store = SETTINGS_GLOBAL;
        fileIndex = GLOBAL_TRAINMODE;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_GENERAL)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_GENERAL;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_LAYOUT)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_LAYOUT;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_PREFERENCES)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_PREFERENCES;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_PRIVATE)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_PRIVATE;
    }

    // and make sure <> text is removed
    return key.remove(QRegularExpression("^<.*>"));

}


// -----------------------------constructor and public instance methods ------------------------//

GSettings::GSettings(QString org, QString app) : newFormat(true){
    credentialSettings = new CredentialSettings(
        createPlatformCredentialStore());
    oldsystemsettings = new QSettings(org,app);
    systemsettings = new QSettings(QSettings::IniFormat, QSettings::UserScope, org, app);
    global = new QVector<QSettings*>();
}

GSettings::GSettings(QString file, QSettings::Format format) : newFormat(false){
    credentialSettings = new CredentialSettings(
        createPlatformCredentialStore());
    systemsettings = new QSettings(file,format);
}

GSettings::~GSettings() {
    syncQSettings();
    if (global) {
        qDeleteAll(*global);
        delete global;
    }
    qDeleteAll(athlete);
    delete oldsystemsettings;
    delete systemsettings;
    delete credentialSettings;
}

QString GSettings::credentialScopeForGlobal()
{
    if (!credentialSettings) return QString();
    if (!newFormat) return credentialScopeForLegacy(QString());
    if (!global || global->isEmpty()) return QString();
    if (globalCredentialScopeId.isEmpty()) {
        const QString preferredScopeId = systemsettings
            ? systemsettings->value(
                  legacyCredentialScopeStorageKey(QString())).toString()
            : QString();
        globalCredentialScopeId = CredentialSettings::ensureScopeId(
            global->at(GLOBAL_GENERAL), credentialScopeStorageKey,
            preferredScopeId);
        mirrorCredentialScope(
            systemsettings, legacyCredentialScopeStorageKey(QString()),
            globalCredentialScopeId);
    }
    return globalCredentialScopeId;
}

QString GSettings::credentialScopeForAthlete(
    const QString &athleteName)
{
    if (!credentialSettings || athleteName.isEmpty()) return QString();
    if (!newFormat) return credentialScopeForLegacy(athleteName);
    const auto found = athlete.constFind(athleteName);
    if (found == athlete.cend()) return QString();
    QString &scopeId = athleteCredentialScopeIds[athleteName];
    if (scopeId.isEmpty()) {
        const QString preferredScopeId = systemsettings
            ? systemsettings->value(
                  legacyCredentialScopeStorageKey(
                      athleteName)).toString()
            : QString();
        scopeId = CredentialSettings::ensureScopeId(
            found.value()->getQSettings(ATHLETE_PRIVATE),
            credentialScopeStorageKey, preferredScopeId);
        mirrorCredentialScope(
            systemsettings, legacyCredentialScopeStorageKey(athleteName),
            scopeId);
    }
    return scopeId;
}

QString GSettings::credentialScopeForLegacy(
    const QString &athleteName)
{
    if (!credentialSettings || !systemsettings) return QString();
    return CredentialSettings::ensureScopeId(
        systemsettings,
        legacyCredentialScopeStorageKey(athleteName));
}

bool GSettings::migrateLegacyCredential(
    const QString &athleteName,
    const QString &credentialKey,
    const QString &storedKey)
{
    if (!CredentialSettings::isCredentialKey(credentialKey)) {
        return false;
    }
    if (!credentialSettings || !oldsystemsettings) return true;

    QString targetKey = credentialKey;
    int store;
    int fileIndex;
    DetermineKey(targetKey, store, fileIndex);
    Q_UNUSED(fileIndex)

    QString scopeId;
    if (store == SETTINGS_GLOBAL) {
        scopeId = credentialScopeForGlobal();
    } else if (store == SETTINGS_ATHLETE) {
        scopeId = credentialScopeForAthlete(athleteName);
    }

    if (!scopeId.isEmpty()) {
        credentialSettings->value(
            oldsystemsettings, scopeId, credentialKey, storedKey,
            QVariant());
    } else {
        CredentialSettings::hardenSettingsFile(oldsystemsettings);
    }
    return true;
}

void GSettings::migrateGlobalCredentials()
{
    if (!credentialSettings || !newFormat
        || !global || global->isEmpty()) return;
    QSettings *settings = global->at(GLOBAL_GENERAL);
    credentialSettings->migratePlaintext(
        settings, credentialScopeForGlobal(),
        QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL));
    for (const QString &key :
         CredentialSettings::credentialKeysForPrefix(
             QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
        migrateValue(key);
    }
}

void GSettings::migrateAthleteCredentials(
    const QString &athleteName)
{
    if (!credentialSettings || !newFormat) return;
    const auto found = athlete.constFind(athleteName);
    if (found == athlete.cend()) return;
    QSettings *settings =
        found.value()->getQSettings(ATHLETE_PRIVATE);
    credentialSettings->migratePlaintext(
        settings, credentialScopeForAthlete(athleteName),
        QStringLiteral(GC_QSETTINGS_ATHLETE_PRIVATE));
    for (const QString &key :
         CredentialSettings::credentialKeysForPrefix(
             QStringLiteral(GC_QSETTINGS_ATHLETE_PRIVATE))) {
        migrateCValue(athleteName, key);
    }
}


QVariant
GSettings::value(const QObject * /*me*/, const QString key, const QVariant def) {

    if (credentialSettings
        && CredentialSettings::isCredentialKey(key)) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            if (store != SETTINGS_GLOBAL
                || !global || global->isEmpty()) {
                return def;
            }
            return credentialSettings->value(
                global->at(file), credentialScopeForGlobal(),
                key, plaintextKey, def);
        }
        if (!key.startsWith(
                QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
            return def;
        }
        return credentialSettings->value(
            systemsettings, credentialScopeForLegacy(QString()),
            key, plaintextKey, def);
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            return systemsettings->value(keyVar, def);
            break;
        case SETTINGS_GLOBAL:
            return global->at(file)->value(keyVar, def);
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "GetValue key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
            break;
        }

    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        return systemsettings->value(keyVar, def);
    }
    return QVariant();
}

void
GSettings::setValue(QString key, QVariant value)
{
    if (credentialSettings
        && CredentialSettings::isCredentialKey(key)) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            if (store == SETTINGS_GLOBAL
                && global && !global->isEmpty()) {
                credentialSettings->setValue(
                    global->at(file), credentialScopeForGlobal(),
                    key, plaintextKey, value);
            }
        } else if (key.startsWith(
                       QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
            credentialSettings->setValue(
                systemsettings, credentialScopeForLegacy(QString()),
                key, plaintextKey, value);
        }
        return;
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            systemsettings->setValue(keyVar,value);
            break;
        case SETTINGS_GLOBAL:
            global->at(file)->setValue(keyVar,value);
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "SetValue key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
            break;

        }
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        systemsettings->setValue(keyVar, value);
    }

}

void
GSettings::remove(const QString &key)
{
    if (credentialSettings
        && CredentialSettings::isCredentialKey(key)) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            if (store == SETTINGS_GLOBAL
                && global && !global->isEmpty()) {
                credentialSettings->remove(
                    global->at(file), credentialScopeForGlobal(),
                    key, plaintextKey);
            }
        } else if (key.startsWith(
                       QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
            credentialSettings->remove(
                systemsettings, credentialScopeForLegacy(QString()),
                key, plaintextKey);
        }
        return;
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            systemsettings->remove(keyVar);
            break;
        case SETTINGS_GLOBAL:
            global->at(file)->remove(keyVar);
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "remove key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
            break;
        }
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        systemsettings->remove(keyVar);
    }
}

// access to athlete specific config
QVariant
GSettings::cvalue(QString athleteName, QString key, QVariant def) {

    if (athleteName.isNull() || athleteName.isEmpty()) return def;

    if (credentialSettings
        && CredentialSettings::isCredentialKey(key)) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            if (store == SETTINGS_GLOBAL) {
                if (!global || global->isEmpty()) return def;
                return credentialSettings->value(
                    global->at(file), credentialScopeForGlobal(),
                    key, plaintextKey, def);
            }
            if (store == SETTINGS_ATHLETE) {
                const auto found = athlete.constFind(athleteName);
                if (found != athlete.cend()) {
                    return credentialSettings->value(
                        found.value()->getQSettings(file),
                        credentialScopeForAthlete(athleteName),
                        key, plaintextKey, def);
                }
                if (oldsystemsettings) {
                    const QString legacyKey =
                        athleteName + QLatin1Char('/') + plaintextKey;
                    return credentialSettings->value(
                        oldsystemsettings,
                        credentialScopeForLegacy(athleteName),
                        key, legacyKey, def);
                }
            }
            return def;
        }

        const bool globalCredential =
            store == SETTINGS_GLOBAL;
        const QString storedKey = globalCredential
            ? plaintextKey
            : athleteName + QLatin1Char('/') + plaintextKey;
        return credentialSettings->value(
            systemsettings,
            credentialScopeForLegacy(
                globalCredential ? QString() : athleteName),
            key, storedKey, def);
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);

        QHash<QString, AthleteQSettings*>::const_iterator i = athlete.find(athleteName);
        if (i != athlete.end()) {
            switch (store) {
            case SETTINGS_SYSTEM:
            case SETTINGS_GLOBAL:
                qDebug() << "GetCValue key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
                break;
            case SETTINGS_ATHLETE:
                return i.value()->getQSettings(file)->value(keyVar, def);
                break;
            }
        } else {
            // fall back to old settings - assuming that this can only happen during the upgrade of an athlete
            // and before the new /config folder exists
            return oldsystemsettings->value(athleteName+"/"+keyVar, def);
        }

    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        return systemsettings->value(athleteName+"/"+keyVar, def);
    }
    return QVariant();

}

void
GSettings::setCValue(QString athleteName, QString key, QVariant value) {

    if (credentialSettings
        && CredentialSettings::isCredentialKey(key)) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            if (store == SETTINGS_GLOBAL
                && global && !global->isEmpty()) {
                credentialSettings->setValue(
                    global->at(file), credentialScopeForGlobal(),
                    key, plaintextKey, value);
            } else if (store == SETTINGS_ATHLETE) {
                const auto found = athlete.constFind(athleteName);
                if (found != athlete.cend()) {
                    credentialSettings->setValue(
                        found.value()->getQSettings(file),
                        credentialScopeForAthlete(athleteName),
                        key, plaintextKey, value);
                }
            }
        } else {
            const bool globalCredential =
                store == SETTINGS_GLOBAL;
            const QString storedKey = globalCredential
                ? plaintextKey
                : athleteName + QLatin1Char('/') + plaintextKey;
            credentialSettings->setValue(
                systemsettings,
                credentialScopeForLegacy(
                    globalCredential ? QString() : athleteName),
                key, storedKey, value);
        }
        return;
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        QHash<QString, AthleteQSettings*>::const_iterator i = athlete.find(athleteName);
        if (i != athlete.end()) {
            switch (store) {
            case SETTINGS_SYSTEM:
            case SETTINGS_GLOBAL:
                qDebug() << "SetCValue keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
                break;
            case SETTINGS_ATHLETE:
                i.value()->getQSettings(file)->setValue(keyVar, value);
                break;
            }
        } // if we do have have the athlete - then we do not store anything
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        systemsettings->setValue(athleteName + "/" + keyVar,value);

    }
}

// other functions unsed from QSettings which GSettings needs to implement
QStringList
GSettings::allKeys() const {

    if (newFormat) {
        QStringList allKeys, tempKeys;
        tempKeys = systemsettings->allKeys();
        foreach (QString key, tempKeys) {
           allKeys.append(GC_QSETTINGS_SYSTEM+key);
        }
        tempKeys = global->at(GLOBAL_GENERAL)->allKeys();
        foreach (QString key, tempKeys) {
           allKeys.append(GC_QSETTINGS_GLOBAL_GENERAL+key);
        }
        tempKeys = global->at(GLOBAL_TRAINMODE)->allKeys();
        foreach (QString key, tempKeys) {
           allKeys.append(GC_QSETTINGS_GLOBAL_TRAIN+key);
        }
        QHashIterator<QString, AthleteQSettings*> i(athlete);
        i.toFront();
        while (i.hasNext())
        { i.next();
            tempKeys = i.value()->getQSettings(ATHLETE_GENERAL)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_GENERAL+key);
            }
            tempKeys = i.value()->getQSettings(ATHLETE_LAYOUT)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_LAYOUT+key);
            }
            tempKeys = i.value()->getQSettings(ATHLETE_PREFERENCES)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_PREFERENCES+key);
            }
            tempKeys = i.value()->getQSettings(ATHLETE_PRIVATE)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_PRIVATE+key);
            }
        }
        allKeys.removeDuplicates();  // remove duplicate keys from the Athlete Settings
        return allKeys;
    } else {
        return systemsettings->allKeys();
    }
    return QStringList();
}

bool
GSettings::contains(const QString & key) const {

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            return systemsettings->contains(keyVar);
            break;
        case SETTINGS_GLOBAL:
            return global->at(file)->contains(keyVar);
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "Contains Value key:" << key << "keyVar:" << keyVar; // error cases on code configuration
            return false;
            break;
        }
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        return systemsettings->contains(keyVar);
    }
    return false;
}

void
GSettings::migrateQSettingsSystem() {

    if (!newFormat) return;

    // do the migration for the System Settings - if not yet done
    // - System is only migrated once per PC (since it only exists once

    bool migrateMac = false;
    QStringList currentKeys = systemsettings->allKeys();
#ifdef Q_OS_MAC
    migrateMac = true;
#endif
    if (currentKeys.size() == 0 || (migrateMac && currentKeys.size() == 1)) {
        upgradeSystem();
        systemsettings->sync();
    }
}


void
GSettings::initializeQSettingsGlobal(QString athletesRootDir) {

    if (!newFormat) return;

    if (global->isEmpty()) {

        global->append(new QSettings(athletesRootDir+"/"+settingFileNamesGlobal[GLOBAL_GENERAL],QSettings::IniFormat));
        global->append(new QSettings(athletesRootDir+"/"+settingFileNamesGlobal[GLOBAL_TRAINMODE],QSettings::IniFormat));

    }

    // do the migration for the AthleteDir / Global Settings  if not yet done
    // - Global is migrated to the root of ANY AthleteDirectory if it does not yet exist
    //   this is too support migration if a user has multiple AthleteDirectories in place
    //   but also creates a default like his previous old-style directory when new AthleteDirectory is created

    if (global->at(GLOBAL_GENERAL)->allKeys().isEmpty() && global->at(GLOBAL_TRAINMODE)->allKeys().isEmpty()) {
        upgradeGlobal();
    }
    syncQSettingsGlobal();
    migrateGlobalCredentials();

}

void
GSettings::initializeQSettingsAthlete(QString athletesRootDir, QString athleteName) {

    // assumption is that the directory "<athletesRootDir>/athleteName" exists //

    if (!newFormat) return;

    // handle not yet upgraded athlete folders without causing problems
    // initializing of the QSettings would work anyway - but upgrade would fail,
    // since the /config folder does not exist - so leave the upgrade for the next
    // initialization after successfull upgrade (the case should be rare anyway)
    QDir configDir(athletesRootDir+"/"+athleteName+"/config");
    if (!configDir.exists()) {
        return; // athlete has not yet been migrated and /config does not exist - so wait until next time
    }

    // create the New Athlete QSettings (if they do not yet exists and migrate the old data if required)

    QHash<QString, AthleteQSettings*>::const_iterator i = athlete.find(athleteName);
    if (i == athlete.end()) {

        initializeQSettingsNewAthlete(athletesRootDir, athleteName);

        QHash<QString, AthleteQSettings*>::const_iterator i2 = athlete.find(athleteName);
        // do the upgrade for the Athlete Properties - but only if the Settings are currently empty - don't overwrite anything
        if (i2 != athlete.end()) {
            if (i2.value()->getQSettings(ATHLETE_GENERAL)->allKeys().isEmpty() &&
                    i2.value()->getQSettings(ATHLETE_LAYOUT)->allKeys().isEmpty() &&
                    i2.value()->getQSettings(ATHLETE_PREFERENCES)->allKeys().isEmpty() &&
                    i2.value()->getQSettings(ATHLETE_PRIVATE)->allKeys().isEmpty() ) {
                upgradeAthlete(athleteName);

            }
        }
        syncQSettingsAllAthletes();
    }
    migrateAthleteCredentials(athleteName);
}

void
GSettings::initializeQSettingsNewAthlete(QString athletesRootDir, QString athleteName) {

    if (!newFormat) return;

    // create the Athlete QSettings - they MUST not exist yet
    AthleteQSettings* athleteSettings = new AthleteQSettings();
    QString baseName = athletesRootDir + "/" + athleteName + "/config/";
    athleteSettings->setQSettings(new QSettings(baseName+settingFileNamesAthlete[ATHLETE_GENERAL], QSettings::IniFormat), ATHLETE_GENERAL );
    athleteSettings->setQSettings(new QSettings(baseName+settingFileNamesAthlete[ATHLETE_LAYOUT], QSettings::IniFormat), ATHLETE_LAYOUT );
    athleteSettings->setQSettings(new QSettings(baseName+settingFileNamesAthlete[ATHLETE_PREFERENCES], QSettings::IniFormat), ATHLETE_PREFERENCES );
    athleteSettings->setQSettings(new QSettings(baseName+settingFileNamesAthlete[ATHLETE_PRIVATE], QSettings::IniFormat), ATHLETE_PRIVATE );
    athlete.insert(athleteName, athleteSettings);

}


void
GSettings::syncQSettingsAllAthletes() {

    if (!newFormat) return;

    QHashIterator<QString, AthleteQSettings*> i(athlete);
    i.toFront();
    while (i.hasNext())
    { i.next();
        i.value()->getQSettings(ATHLETE_GENERAL)->sync();
        i.value()->getQSettings(ATHLETE_LAYOUT)->sync();
        i.value()->getQSettings(ATHLETE_PREFERENCES)->sync();
        i.value()->getQSettings(ATHLETE_PRIVATE)->sync();
        CredentialSettings::hardenSettingsFile(
            i.value()->getQSettings(ATHLETE_PRIVATE));
    }
}

void
GSettings::syncQSettingsGlobal() {

    if (!newFormat) return;

    if (global->size() == 2) {
        global->at(GLOBAL_GENERAL)->sync();
        global->at(GLOBAL_TRAINMODE)->sync();
        CredentialSettings::hardenSettingsFile(
            global->at(GLOBAL_GENERAL));
    };
}

void
GSettings::syncQSettings() {

    systemsettings->sync();
    CredentialSettings::hardenSettingsFile(systemsettings);
    syncQSettingsGlobal();
    syncQSettingsAllAthletes();

}

void
GSettings::clearGlobalAndAthletes() {

    if (!newFormat) return;
    syncQSettings();
    qDeleteAll(*global);
    qDeleteAll(athlete);
    global->clear();
    athlete.clear();
    globalCredentialScopeId.clear();
    athleteCredentialScopeIds.clear();
    if (credentialSettings) credentialSettings->clearCache();
}


/*-------------------------------- special methods for Upgrade/Migration --------------------------
 *
 * The .INI based storage of Settings has been introduced with GoldenCheetah v3.3.0
 *
 * To transition existing settings (in PLISTs (OSX) and Registry (WINDOWS) from the
 * propriety storage to the common .INI files an automatic migration of Settings takes
 * place when no Settings are found. The methods executing the migration are implemented here
 *
 * Any development starting starting after v3.3 (so v4.0 and onwards) does not need
 * to take the migration into account, since any newly defined settings are only stored
 * using the new .INI based technique.
 *
 -----------------------------------------------------------------------------------------------*/

void
GSettings::migrateValue(QString key) {

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    if (oldsystemsettings->contains(oldKey)
        && !migrateLegacyCredential(QString(), key, oldKey)) {
        setValue(key, oldsystemsettings->value(oldKey));
    }
}

void
GSettings::migrateCValue(QString athlete, QString key) {

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    const QString storedKey = athlete + QLatin1Char('/') + oldKey;
    if (oldsystemsettings->contains(storedKey)
        && !migrateLegacyCredential(athlete, key, storedKey)) {
        setCValue(athlete, key, oldsystemsettings->value(storedKey));
    }
}

void
GSettings::migrateAndRenameCValue(QString athlete, QString wrongKey, QString key) {

    wrongKey.remove(QRegularExpression("^<.*>"));
    const QString storedKey = athlete + QLatin1Char('/') + wrongKey;
    if (oldsystemsettings->contains(storedKey)
        && !migrateLegacyCredential(athlete, key, storedKey)) {
        setCValue(athlete, key, oldsystemsettings->value(storedKey));
    }
}

void
GSettings::migrateValueToCValue(QString athlete, QString key) {

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    if (oldsystemsettings->contains(oldKey)
        && !migrateLegacyCredential(athlete, key, oldKey)) {
        setCValue(athlete, key, oldsystemsettings->value(oldKey));
    }
}

void
GSettings::migrateCValueToValue(QString athlete, QString key) {

    // only migrate if the value does not yet exist on the target INI file
    if (!contains(key)) {
        QString oldKey = key;
        oldKey.remove(QRegularExpression("^<.*>"));
        oldKey = athlete + QLatin1Char('/') + oldKey;
        if (oldsystemsettings->contains(oldKey)
            && !migrateLegacyCredential(athlete, key, oldKey)) {
            setValue(key, oldsystemsettings->value(oldKey));
        }
    }
}


void
GSettings::upgradeSystem() {

    // by explicitely naming all the properties, and not choosing the "allKeys()" function,
    // only the properties still in use are migrated - and not any orphans for previous releases

    // NOTE: Migrating values is only required for settings introduced in GC version until v3.3

    migrateValue(GC_HOMEDIR);
    migrateValue(GC_SETTINGS_LAST);
    migrateValue(GC_SETTINGS_MAIN_GEOM);
    migrateValue(GC_SETTINGS_MAIN_STATE);
    migrateValue(GC_SETTINGS_LAST_IMPORT_PATH);
    migrateValue(GC_SETTINGS_LAST_WORKOUT_PATH);
    migrateValue(GC_LAST_DOWNLOAD_DEVICE);
    migrateValue(GC_LAST_DOWNLOAD_PORT);
    migrateValue(GC_BE_LASTDIR);
    migrateValue(GC_BE_LASTFMT);
    migrateValue(GC_FONT_DEFAULT);
    migrateValue(GC_FONT_DEFAULT_SIZE);
    migrateValue(GC_FONT_CHARTLABELS);
    migrateValue(GC_FONT_CHARTLABELS_SIZE);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_TITLES);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CHARTMARKERS);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CALENDAR);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_TITLES_SIZE);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CHARTMARKERS_SIZE);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CALENDAR_SIZE);

    QStringList colorProperties = GCColor::getConfigKeys();
    QStringListIterator colorIterator(colorProperties);
    while (colorIterator.hasNext()) {
        QString key = QString(colorIterator.next().data());
        migrateValue(key);
    }
}

void
GSettings::upgradeGlobal() {

    // by explicitely naming all the properties, and not choosing the "allKeys()" function,
    // only the properties still in use are migrated - and not any orphans for previous releases

    // NOTE: Migrating values is only required for settings introduced in GC version until v3.3
    migrateValue(GC_SETTINGS_FAVOURITE_METRICS);
    migrateValue(GC_TABBAR);
    migrateValue(GC_WBALFORM);
    migrateValue(GC_BIKESCOREDAYS);
    migrateValue(GC_BIKESCOREMODE);
    migrateValue(GC_WARNCONVERT);
    migrateValue(GC_WARNEXIT);
    migrateValue(GC_HIST_BIN_WIDTH);
    migrateValue(GC_WORKOUTDIR);
    migrateValue(GC_LINEWIDTH);
    migrateValue(GC_ANTIALIAS);
    migrateValue(GC_RIDESCROLL);
    migrateValue(GC_RIDEHEAD);
    migrateValue(GC_SHADEZONES);
    migrateValue(GC_GARMIN_SMARTRECORD);
    migrateValue(GC_GARMIN_HWMARK);
    migrateValue(GC_DPFG_TOLERANCE);
    migrateValue(GC_DPFG_STOP);
    migrateValue(GC_DPFS_MAX);
    migrateValue(GC_DPFS_VARIANCE);
    migrateValue(GC_DPTA);
    migrateValue(GC_DPPA);
    migrateValue(GC_DPFHRS_MAX);
    migrateValue(GC_DPDP_BIKEWEIGHT);
    migrateValue(GC_DPDP_CRR);
    migrateValue(GC_LANG);
    migrateValue(GC_PACE);
    migrateValue(GC_SWIMPACE);
    migrateValue(GC_ELEVATION_HYSTERESIS);
    migrateValue(GC_START_HTTP);

    // Handle the Dataprocessor dp/%1/apply keys
    // Handle the RideEditor colmap/%1 keys
    QStringList dpKeys = oldsystemsettings->allKeys();
    QStringListIterator dpKeysIterator(dpKeys);
    while (dpKeysIterator.hasNext()) {
        QString key = QString(dpKeysIterator.next().data());
        if (key.startsWith("dp/") || key.startsWith("colmap/")) {
            migrateValue(GC_QSETTINGS_GLOBAL_GENERAL+key);
        }
    }

    // handle the Device configuration
    migrateValue(GC_DEV_COUNT);
    QString devCountKey = GC_DEV_COUNT;
    devCountKey.remove(QRegularExpression("^<.*>"));
    QVariant configVal = oldsystemsettings->value(devCountKey);
    int devicecount;
    if (configVal.isNull()) {
        devicecount=0;
    } else {
        devicecount = configVal.toInt();
    }
    for (int i = 0; i < devicecount; i++ ) {
        QString configStr = QString("%1%2").arg(GC_DEV_NAME).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_TYPE).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_WHEEL).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_SPEC).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_PROF).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_VIRTUAL).arg(i+1);
        migrateValue(configStr);
    }

    migrateValue(FORTIUS_FIRMWARE);
    migrateValue(TRAIN_MULTI);

}


void
GSettings::upgradeAthlete(QString athlete) {

    // by explicitely naming all the properties, and not choosing the "allKeys()" function,
    // only the properties still in use are migrated - and not any orphans for previous releases

    // NOTE: Migrating values is only required for settings introduced in GC version until v3.3

    migrateCValue(athlete, GC_VERSION_USED);
    migrateCValue(athlete, GC_SAFEEXIT);
    migrateCValue(athlete, GC_UPGRADE_FOLDER_SUCCESS);
    migrateCValue(athlete, GC_LTM_LAST_DATE_RANGE);
    migrateCValue(athlete, GC_LTM_AUTOFILTERS);
    migrateCValue(athlete, GC_BLANK_ANALYSIS);
    migrateCValue(athlete, GC_BLANK_TRAIN);
    migrateCValue(athlete, GC_BLANK_HOME);
    migrateCValue(athlete, GC_BLANK_PLAN);
    migrateCValue(athlete, GC_NICKNAME);
    migrateCValue(athlete, GC_DOB);
    migrateCValue(athlete, GC_WEIGHT);
    migrateCValue(athlete, GC_HEIGHT);
    migrateCValue(athlete, GC_WBALTAU);
    migrateCValue(athlete, GC_SEX);
    migrateCValue(athlete, GC_BIO);
    migrateCValue(athlete, GC_AVATAR);
    migrateCValue(athlete, GC_DISCOVERY);
    migrateCValue(athlete, GC_SB_TODAY);
    migrateCValue(athlete, GC_LTS_DAYS);
    migrateCValue(athlete, GC_STS_DAYS);
    migrateCValue(athlete, GC_NAVHEADINGS);
    migrateCValue(athlete, GC_NAVGROUPBY);
    migrateCValue(athlete, GC_SORTBY);
    migrateCValue(athlete, GC_WEBCAL_URL);
    migrateCValue(athlete, GC_USE_CP_FOR_FTP);

    migrateAndRenameCValue(athlete, "bavigator/headingwidths", GC_NAVHEADINGWIDTHS);
    migrateCValueToValue(athlete, GC_UNIT);

    // Handle the splittersizes keys
    QStringList splitterKeys = oldsystemsettings->allKeys();
    QStringListIterator splitterKeysIterator(splitterKeys);
    while (splitterKeysIterator.hasNext()) {
        QString key = QString(splitterKeysIterator.next().data());
        if (key.startsWith(athlete + "/mainwindow/splitterSizes") || key.startsWith(athlete+"/splitter")) {
            key.remove(0, athlete.size()+1); // remove the Athlete name and / from the old setting !
            migrateCValue(athlete, GC_QSETTINGS_ATHLETE_LAYOUT+key);
        }
    }

    // --- private --- //
    migrateCValue(athlete, GC_RWGPSUSER);
    migrateCValue(athlete, GC_RWGPSPASS);
    migrateCValue(athlete, GC_WIURL);
    migrateCValue(athlete, GC_WIUSER);
    migrateCValue(athlete, GC_WIKEY);
    migrateCValue(athlete, GC_DVURL);
    migrateCValue(athlete, GC_DVUSER);
    migrateCValue(athlete, GC_DVPASS);
    migrateCValue(athlete, GC_DVCALDAVTYPE);
    migrateCValue(athlete, GC_STRAVA_TOKEN);
    migrateCValue(athlete, GC_CYCLINGANALYTICS_TOKEN);

    // migrate from system/global to athlete specific settings
    migrateValueToCValue(athlete, GC_CRANKLENGTH);
    migrateValueToCValue(athlete, GC_WHEELSIZE);

}

static QString fontfamilyfallback[] = {
#ifdef Q_OS_LINUX
    // try pretty fonts first (you never know)
    "Noto Sans Display", // google free font
    "Clear Sans", // intel free font
    "DejaVu Sans", // gnome free font
    "Liberation Sans", // red hat free font

    // then distro specific ones
    "Ubuntu",
    "Red Hat Display",

#endif
#ifdef Q_OS_WIN
    "Segoe UI",
    "Calibri",
    "Microsoft Sans Serif",
#endif
#ifdef Q_OS_MAC
    "SF Pro Display",
    "PT Sans",
    "Helvetica Neue",
#endif

    // common fonts
    "Trebuchet MS",
    "Helvetica",

    // on all OS these two should exist at a minimum
    "Verdana",
    "Arial",
    NULL
};


// font selection and scaling uses slightly smaller fonts on MacOS
#ifdef Q_OS_MAC
#define FONTROWS 48
#else
#define FONTROWS 43
#endif

AppearanceSettings
GSettings::defaultAppearanceSettings()
{
    AppearanceSettings returning;

    // lets get the geometry of the window next
    // since its used to scale and set other
    // appearance settings
    QRect screensize = QApplication::primaryScreen()->availableGeometry();

    // leave 12% of the screen free to the left and right of the main window
    // and same number of pixels above and below
    double width = screensize.width() * 0.88;
    double margin = (screensize.width() - width) / 2.0;
    returning.windowsize.setWidth(screensize.width() - margin);
    returning.windowsize.setHeight(screensize.height() - margin);
    returning.windowsize.setX(margin);
    returning.windowsize.setY(margin);

    // sidebars should be about 20% of width and no more
    returning.sidebarwidth = returning.windowsize.width() / 5;

    // lets find an appropriate font
    returning.fontfamily = QFont().toString(); // ultimately fall back to QT default
    QFontDatabase fontdb;
    for(int i=0; !fontfamilyfallback[i].isEmpty(); i++) {

        foreach(QString family, fontdb.families()) {

            // is it installed ?
            if (family == fontfamilyfallback[i]) {
                returning.fontfamily = fontfamilyfallback[i];
                goto breakout;
            }
        }
    }

breakout:

    returning.fontpointsize = 11; // default

    // scaling only applies on hidpi displays
    returning.fontscale = 1.0;
    returning.fontscaleindex = 4;
    returning.xfactor = 1.0;
    returning.yfactor = 1.0;

    // dpiXFactor and dpiYFactor are used to scale across the code
    // typically to increase the size of widgets but also some other
    // graphical elements
    if (QApplication::primaryScreen()->devicePixelRatio() <= 1 && screensize.width() > 2160) {
       // we're on a hidpi screen - lets create a multiplier - always use smallest
       returning.xfactor = screensize.width() / 1280.0;
       returning.yfactor = screensize.height() / 1024.0;

        // always make the same, use smallest scaling when x and y differ
       if (returning.yfactor < returning.xfactor) returning.xfactor = returning.yfactor;
       else if (returning.xfactor < returning.yfactor) returning.yfactor = returning.xfactor;

    }

    // we also need to make sure fonts are scaled to be large/small enough
    // to use the screen estate reasonably- whilst some users will prefer
    // small fonts, we scale to a size that looks the same on all resolutions
    // and avoid overly small fonts. Users can of course adjust the scaling
    // to their own preferences later
    for (int i=0; scalefactors[i] != 0; i++) {

        QFont font(returning.fontfamily);
        font.setPointSizeF(returning.fontpointsize * scalefactors[i]);
        QFontMetricsF metrics(font);
        double height = metrics.boundingRect("TEST").height();

        if (returning.windowsize.height() / height < FONTROWS) {
            returning.fontscale = scalefactors[i];
            returning.fontscaleindex = i;
            break;
        }
    }

    // best settings for UI as now designed
    returning.theme = 5; // team purple colors
    returning.antialias = true;
    returning.macForms = true;
    returning.scrollbar = true;
    returning.head = false;
    returning.sideanalysis = false;
    returning.sidetrend = false;
    returning.sideplan = false;
    returning.sidetrain = true;

    // linewidth must be wholly divisible by 0.5
    // default is historically 2px but 4px is too thick
    // on hidpi displays typically, so we adjust to 3px
    returning.linewidth = dpiXFactor > 1 ? 1.5 * dpiXFactor : 2.0;
    double factor = returning.linewidth / 0.5;
    factor=qRound(factor);
    returning.linewidth = 0.5 * factor;

    return returning;
}

//----------------------------------------------------------------------------------------------//

// initialise with no athlete
GSettings *appsettings = GetApplicationSettings();
