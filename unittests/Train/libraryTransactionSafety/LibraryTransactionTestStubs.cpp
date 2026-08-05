#include "LibraryTransactionTestStubs.h"

#include <QFile>

QList<Library *> libraries;
TestAppSettings *appsettings = nullptr;
bool LibraryParser::serializeResult = true;
bool LibraryParser::writeBeforeReturning = true;
int LibraryParser::serializeCalls = 0;

bool LibraryParser::serialize(QDir home, QString *error)
{
    ++serializeCalls;
    if (writeBeforeReturning) {
        home.cdUp();
        QFile file(home.filePath(QStringLiteral("library.xml")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error != nullptr) *error = file.errorString();
            return false;
        }
        for (Library *library : libraries) {
            if (library == nullptr) continue;
            for (const QString &ref : library->refs) {
                file.write(ref.toUtf8());
                file.write("\n");
            }
        }
        file.close();
    }
    if (!serializeResult && error != nullptr) {
        *error = QStringLiteral("injected library serialization failure");
    }
    return serializeResult;
}

void LibraryParser::reset()
{
    serializeResult = true;
    writeBeforeReturning = true;
    serializeCalls = 0;
}

Library *Library::findLibrary(QString name)
{
    for (Library *library : libraries) {
        if (library != nullptr && library->name == name) {
            return library;
        }
    }
    return nullptr;
}

ErgFile::ErgFile(const QString &path, ErgFileFormat, Context *)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    bool powerOk = false;
    const double averagePower = QString::fromUtf8(file.readAll()).trimmed()
                                    .toDouble(&powerOk);
    if (!powerOk) {
        return;
    }

    valid = true;
    format(ErgFileFormat::erg);
    filename(path);
    name(QFileInfo(path).baseName());
    AP(averagePower);
}

bool ErgFile::isWorkout(const QString &path)
{
    return QFileInfo(path).suffix().compare(
               QStringLiteral("erg"), Qt::CaseInsensitive) == 0;
}

VideoSyncFile::VideoSyncFile(const QString &path, int &, Context *)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)
        || file.readAll().trimmed() != QByteArrayLiteral("videosync")) {
        return;
    }

    valid = true;
    format(VideoSyncFileFormat::rlv);
    filename(path);
    name(QFileInfo(path).baseName());
    source(QStringLiteral("test"));
}

bool VideoSyncFile::isVideoSync(const QString &path)
{
    return QFileInfo(path).suffix().compare(
               QStringLiteral("rlv"), Qt::CaseInsensitive) == 0;
}
