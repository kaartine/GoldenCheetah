#include <QtTest>

#include "AnchoredFileSystem.h"
#include "SplitActivitySave.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <cstddef>
#include <cstring>
#include <qt_windows.h>
#include <winioctl.h>
#endif

namespace {

constexpr int SplitCrashExitCode = 91;
constexpr auto CrashRootEnvironment = "GC_SPLIT_CRASH_ROOT";
constexpr auto CrashModeEnvironment = "GC_SPLIT_CRASH_MODE";
constexpr auto CrashPhaseEnvironment = "GC_SPLIT_CRASH_PHASE";
constexpr auto CrashOccurrenceEnvironment =
    "GC_SPLIT_CRASH_OCCURRENCE";
constexpr auto CrashKeepOriginalEnvironment =
    "GC_SPLIT_CRASH_KEEP_ORIGINAL";

QString sourceFileName()
{
    return QStringLiteral("2026_07_06_08_00_00.json");
}

QString firstOutputName()
{
    return QStringLiteral("2026_07_06_08_30_00.json");
}

QString secondOutputName()
{
    return QStringLiteral("2026_07_06_09_00_00.json");
}

QByteArray sourceContents()
{
    return QByteArray("source-version-one");
}

bool filePresent(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool writeBytes(
    const QString &path,
    const QByteArray &contents,
    QString &error)
{
    error.clear();
    QFile file(path);
    if (!file.open(
            QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString();
        return false;
    }
    if (file.write(contents)
            != static_cast<qint64>(contents.size())
        || !file.flush()) {
        error = file.errorString().isEmpty()
            ? QStringLiteral("short write")
            : file.errorString();
        return false;
    }
    return true;
}

bool createDirectoryLink(
    const QString &target,
    const QString &link)
{
#ifdef Q_OS_WIN
    struct ReparseData {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        WCHAR path[1];
    };
    const QString printName = QDir::toNativeSeparators(
        QFileInfo(target).absoluteFilePath());
    const QString substituteName =
        printName.startsWith(QStringLiteral("\\\\"))
        ? QStringLiteral("\\??\\UNC\\") + printName.mid(2)
        : QStringLiteral("\\??\\") + printName;
    const qsizetype substituteBytes =
        substituteName.size() * qsizetype(sizeof(WCHAR));
    const qsizetype printBytes =
        printName.size() * qsizetype(sizeof(WCHAR));
    const qsizetype totalBytes =
        qsizetype(offsetof(ReparseData, path))
        + substituteBytes + qsizetype(sizeof(WCHAR))
        + printBytes + qsizetype(sizeof(WCHAR));
    if (totalBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        return false;
    }

    const QString nativeLink = QDir::toNativeSeparators(link);
    if (!::CreateDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
            nullptr)) {
        return false;
    }
    QByteArray storage(totalBytes, '\0');
    auto *data = reinterpret_cast<ReparseData *>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = WORD(totalBytes - 8);
    data->substituteLength = WORD(substituteBytes);
    data->printOffset = WORD(substituteBytes + sizeof(WCHAR));
    data->printLength = WORD(printBytes);
    char *buffer = reinterpret_cast<char *>(data->path);
    std::memcpy(
        buffer, substituteName.utf16(), size_t(substituteBytes));
    std::memcpy(
        buffer + data->printOffset,
        printName.utf16(),
        size_t(printBytes));

    HANDLE handle = ::CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ::RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
        return false;
    }
    DWORD returned = 0;
    const bool created = ::DeviceIoControl(
        handle,
        FSCTL_SET_REPARSE_POINT,
        data,
        DWORD(totalBytes),
        nullptr,
        0,
        &returned,
        nullptr);
    ::CloseHandle(handle);
    if (!created) {
        ::RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
    }
    return created;
#else
    return QFile::link(target, link)
        && QFileInfo(link).isSymLink();
#endif
}

void writeFixture(
    const QString &path,
    const QByteArray &contents)
{
    QString error;
    QVERIFY2(
        writeBytes(path, contents, error),
        qPrintable(error));
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

struct Fixture
{
    Fixture()
        : activities(
              temporary.filePath(
                  QStringLiteral("activities"))),
          sourcePath(
              activities.filePath(sourceFileName())),
          backupPath(
              temporary.filePath(
                  QStringLiteral(
                      "backup/original-activity.json.bak")))
    {
    }

    bool initialize()
    {
        return temporary.isValid()
            && QDir().mkpath(activities.absolutePath())
            && QDir().mkpath(
                QFileInfo(backupPath).absolutePath());
    }

    QString transactionNamespace() const
    {
        return temporary.filePath(
            QStringLiteral(
                ".gc-transactions/split-activity"));
    }

    QTemporaryDir temporary;
    QDir activities;
    QString sourcePath;
    QString backupPath;
};

SplitActivityOutput outputWriting(
    const QString &fileName,
    const QByteArray &contents,
    int *stageCalls = nullptr)
{
    return SplitActivityOutput{
        fileName,
        [contents, stageCalls](
            QByteArray &staged,
            QString &error) {
            Q_UNUSED(error)
            if (stageCalls) ++*stageCalls;
            staged = contents;
            return true;
        }};
}

bool splitNamespaceIsEmpty(const Fixture &fixture)
{
    const QDir directory(fixture.transactionNamespace());
    return !directory.exists()
        || directory.entryList(
               QDir::AllEntries | QDir::Hidden
                   | QDir::System | QDir::NoDotAndDotDot)
               .isEmpty();
}

QString onlyJournalPath(const Fixture &fixture)
{
    const QDir nameSpace(fixture.transactionNamespace());
    const QFileInfoList journals = nameSpace.entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot);
    return journals.size() == 1
        ? journals.constFirst().absoluteFilePath()
        : QString();
}

bool replaceManifestFileIdentity(
    const Fixture &fixture,
    const QString &recordName,
    const QString &filePath,
    QString &error)
{
    AnchoredFileSystem::DirectoryAnchor parent;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            QFileInfo(filePath).absolutePath(), parent, error)) {
        return false;
    }
    const AnchoredFileSystem::EntryRef entry =
        parent.entry(QFileInfo(filePath).fileName(), error);
    AnchoredFileSystem::PinnedFile file;
    if (!entry.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            entry, file, error)) {
        return false;
    }

    const QString journalPath = onlyJournalPath(fixture);
    if (journalPath.isEmpty()) {
        error = QStringLiteral("cannot locate the split journal");
        return false;
    }
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        error = manifestFile.errorString();
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(
        manifestFile.readAll(), &parseError);
    manifestFile.close();
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral("cannot parse the split manifest");
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject record = root.value(recordName).toObject();
    const QByteArray currentGeneration = file.durableGeneration();
    if (currentGeneration.isEmpty()) {
        error = QStringLiteral(
            "filesystem generation evidence is unavailable");
        return false;
    }
    QByteArray recordedGeneration = QByteArray::fromHex(
        record.value(QStringLiteral("original_generation"))
            .toString().toLatin1());
    if (recordedGeneration == currentGeneration) {
        recordedGeneration += QByteArrayLiteral(":prior");
    }
    record.insert(
        QStringLiteral("original_identity"),
        QString::fromLatin1(file.identity().serializedKey().toHex()));
    record.insert(
        QStringLiteral("original_generation"),
        QString::fromLatin1(recordedGeneration.toHex()));
    root.insert(recordName, record);
    return writeBytes(
        manifestPath,
        QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n',
        error);
}

bool replaceManifestSourceIdentity(
    const Fixture &fixture,
    QString &error)
{
    return replaceManifestFileIdentity(
        fixture,
        QStringLiteral("source"),
        fixture.sourcePath,
        error);
}

bool replaceManifestOutputIdentity(
    const Fixture &fixture,
    const QString &outputName,
    QString &error)
{
    AnchoredFileSystem::DirectoryAnchor activities;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            fixture.activities.absolutePath(), activities, error)) {
        return false;
    }
    const AnchoredFileSystem::EntryRef outputEntry =
        activities.entry(outputName, error);
    AnchoredFileSystem::PinnedFile output;
    if (!outputEntry.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            outputEntry, output, error)) {
        return false;
    }

    const QString journalPath = onlyJournalPath(fixture);
    if (journalPath.isEmpty()) {
        error = QStringLiteral("cannot locate the split journal");
        return false;
    }
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        error = manifestFile.errorString();
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(
        manifestFile.readAll(), &parseError);
    manifestFile.close();
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral("cannot parse the split manifest");
        return false;
    }

    QJsonObject root = document.object();
    QJsonArray outputs = root.value(
        QStringLiteral("outputs")).toArray();
    if (outputs.size() != 1 || !outputs.at(0).isObject()) {
        error = QStringLiteral("cannot locate the split output record");
        return false;
    }
    QJsonObject outputRecord = outputs.at(0).toObject();
    outputRecord.insert(
        QStringLiteral("publication_stage_identity"),
        QString::fromLatin1(output.identity().serializedKey().toHex()));
    outputs.replace(0, outputRecord);
    root.insert(QStringLiteral("outputs"), outputs);
    return writeBytes(
        manifestPath,
        QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n',
        error);
}

bool replaceJournalPayload(
    const Fixture &fixture,
    const QString &name,
    const QByteArray &contents,
    QString &error)
{
    const QString journalPath = onlyJournalPath(fixture);
    if (journalPath.isEmpty()) {
        error = QStringLiteral("cannot locate the split journal");
        return false;
    }
    const QString path = QDir(journalPath).filePath(name);
    if (!QFile::remove(path)) {
        error = QStringLiteral("cannot remove journal payload %1").arg(path);
        return false;
    }
    return writeBytes(path, contents, error);
}

bool alterManifestOutputStoredGeneration(
    const Fixture &fixture,
    QString &error)
{
    const QString journalPath = onlyJournalPath(fixture);
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(
        file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral("cannot parse the split manifest");
        return false;
    }
    QJsonObject root = document.object();
    QJsonArray outputs = root.value(QStringLiteral("outputs")).toArray();
    if (outputs.size() != 1 || !outputs.at(0).isObject()) {
        error = QStringLiteral("cannot locate the split output record");
        return false;
    }
    QJsonObject output = outputs.at(0).toObject();
    QString generation = output.value(
        QStringLiteral("stored_generation")).toString();
    generation.replace(
        generation.size() - 2,
        2,
        generation.endsWith(QStringLiteral("00"))
            ? QStringLiteral("01") : QStringLiteral("00"));
    output.insert(QStringLiteral("stored_generation"), generation);
    outputs.replace(0, output);
    root.insert(QStringLiteral("outputs"), outputs);
    return writeBytes(
        manifestPath,
        QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n',
        error);
}

bool createIntentOnlyJournal(
    const Fixture &fixture,
    QString &error,
    int artifactCount = 1,
    bool distinctParents = false)
{
    const QString id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString journalPath = QDir(fixture.transactionNamespace())
        .filePath(id);
    if (!QDir().mkpath(journalPath)) {
        error = QStringLiteral("cannot create a split intent journal");
        return false;
    }
    QJsonObject intent;
    intent.insert(QStringLiteral("schema"), 1);
    intent.insert(QStringLiteral("id"), id);
    QJsonArray artifacts;
    for (int index = 0; index < artifactCount; ++index) {
        QString parent = QStringLiteral("activities");
        if (distinctParents) {
            parent += QStringLiteral("/intent-%1")
                .arg(index, 4, 10, QLatin1Char('0'));
            if (!QDir(fixture.temporary.path()).mkpath(parent)) {
                error = QStringLiteral(
                    "cannot create split intent artifact parent");
                return false;
            }
        }
        artifacts.append(
            QStringLiteral("%1/.gc-split-%2-output-%3.stage")
                .arg(parent, id)
                .arg(index, 4, 10, QLatin1Char('0')));
    }
    intent.insert(QStringLiteral("artifacts"), artifacts);
    return writeBytes(
        QDir(journalPath).filePath(QStringLiteral("intent.json")),
        QJsonDocument(intent).toJson(QJsonDocument::Compact) + '\n',
        error);
}

bool splitProductionArtifactsAreAbsent(const Fixture &fixture)
{
    QDirIterator entries(
        fixture.temporary.path(),
        QDir::AllEntries | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString name = QFileInfo(entries.next()).fileName();
        if (name.startsWith(QStringLiteral(".gc-split-"))
            || name.startsWith(QStringLiteral(".gc-remove-"))) {
            return false;
        }
    }
    return true;
}

enum class SplitInjection
{
    None,
    CreateTargetBeforePublish,
    ReplacePublishedTargetAndFailSync,
    FailExplicitSync,
    LeaveCommittedJournal
};

SplitInjection splitInjection = SplitInjection::None;
QString injectedTargetPath;
QString injectedSyncParent;
QByteArray injectedTargetContents;
bool injectionSucceeded = false;
bool explicitSyncReached = false;
bool failNextAnchoredSync = false;
bool anchoredSyncFailureConsumed = false;
QString injectedTransactionRoot;
QString injectedJournalBlocker;
bool requireSameParentMoves = false;
QString injectedUnlinkPathFragment;
int injectedUnlinkFailuresRemaining = 0;
QStringList enumeratedDirectories;
int liveAnchoredStates = 0;
int peakAnchoredStates = 0;
qint64 recoveryByteLimit = -1;
qint64 recoveryOperationLimit = -1;
qint64 recoveryDeadlineMilliseconds = -1;
qint64 recoveryDeadlineAfterBytes = -1;
int recoveryBudgetStarts = 0;
qint64 recoveryBytesConsumed = 0;
qint64 pathValidationSteps = 0;
bool forceGenerationUnavailable = false;

void resetInjection()
{
    splitInjection = SplitInjection::None;
    injectedTargetPath.clear();
    injectedSyncParent.clear();
    injectedTargetContents.clear();
    injectionSucceeded = false;
    explicitSyncReached = false;
    failNextAnchoredSync = false;
    anchoredSyncFailureConsumed = false;
    injectedTransactionRoot.clear();
    injectedJournalBlocker.clear();
    requireSameParentMoves = false;
    injectedUnlinkPathFragment.clear();
    injectedUnlinkFailuresRemaining = 0;
    enumeratedDirectories.clear();
    liveAnchoredStates = 0;
    peakAnchoredStates = 0;
    recoveryByteLimit = -1;
    recoveryOperationLimit = -1;
    recoveryDeadlineMilliseconds = -1;
    recoveryDeadlineAfterBytes = -1;
    recoveryBudgetStarts = 0;
    recoveryBytesConsumed = 0;
    pathValidationSteps = 0;
    forceGenerationUnavailable = false;
}

QJsonObject snapshotRecord(
    const QString &path,
    bool exists,
    const QString &stored)
{
    QJsonObject object;
    object.insert(QStringLiteral("path"), path);
    object.insert(QStringLiteral("exists"), exists);
    object.insert(
        QStringLiteral("size"),
        exists ? QStringLiteral("1") : QStringLiteral("0"));
    object.insert(
        QStringLiteral("sha256"),
        exists ? QString(64, QLatin1Char('0')) : QString());
    object.insert(
        QStringLiteral("original_identity"),
        exists ? QStringLiteral("66736f75726365") : QString());
    object.insert(
        QStringLiteral("original_generation"),
        exists ? QStringLiteral("67656e65726174696f6e") : QString());
    object.insert(QStringLiteral("stored"), stored);
    object.insert(
        QStringLiteral("stored_identity"),
        exists
            ? QStringLiteral("66736f757263652d636f7079")
            : QString());
    object.insert(
        QStringLiteral("stored_generation"),
        exists ? QStringLiteral("73746f7265642d67656e") : QString());
    object.insert(QStringLiteral("archive_stage_path"), QString());
    object.insert(
        QStringLiteral("archive_stage_identity"), QString());
    object.insert(
        QStringLiteral("archive_stage_generation"), QString());
    object.insert(QStringLiteral("retired_path"), QString());
    return object;
}

QJsonObject outputRecord(
    int index, const QString &path, const QString &id)
{
    QJsonObject object;
    const QString outputDirectory = QFileInfo(path).path();
    object.insert(QStringLiteral("path"), path);
    object.insert(
        QStringLiteral("stored"),
        QStringLiteral("output-%1.new")
            .arg(index, 4, 10, QLatin1Char('0')));
    object.insert(QStringLiteral("size"), QStringLiteral("1"));
    object.insert(
        QStringLiteral("sha256"),
        QString(64, QLatin1Char('0')));
    object.insert(
        QStringLiteral("stored_identity"),
        QStringLiteral("66%1")
            .arg(index + 1, 4, 16, QLatin1Char('0')));
    object.insert(
        QStringLiteral("stored_generation"),
        QStringLiteral("73746f7265642d%1")
            .arg(index + 1, 4, 16, QLatin1Char('0')));
    object.insert(
        QStringLiteral("publication_stage_path"),
        QDir(outputDirectory).filePath(
            QStringLiteral(".gc-split-%1-output-%2.stage")
                .arg(id)
                .arg(index, 4, 10, QLatin1Char('0'))));
    object.insert(
        QStringLiteral("publication_stage_identity"),
        QStringLiteral("66aa%1")
            .arg(index + 1, 4, 16, QLatin1Char('0')));
    object.insert(
        QStringLiteral("publication_stage_generation"),
        QStringLiteral("67656e2d%1")
            .arg(index + 1, 4, 16, QLatin1Char('0')));
    return object;
}

QJsonObject baseManifest(const QString &id)
{
    QJsonObject object;
    object.insert(QStringLiteral("schema"), 6);
    object.insert(QStringLiteral("id"), id);
    object.insert(
        QStringLiteral("state"),
        QStringLiteral("prepared"));
    object.insert(QStringLiteral("keepOriginal"), true);
    object.insert(
        QStringLiteral("source"),
        snapshotRecord(
            QStringLiteral(
                "activities/2026_07_06_08_00_00.json"),
            true,
            QStringLiteral("source.old")));
    object.insert(
        QStringLiteral("backup"),
        snapshotRecord(QString(), false, QString()));
    QJsonArray outputs;
    outputs.append(
        outputRecord(
            0,
            QStringLiteral(
                "activities/2026_07_06_08_30_00.json"),
            id));
    object.insert(QStringLiteral("outputs"), outputs);
    return object;
}

bool createHostileJournal(
    const Fixture &fixture,
    const QString &id,
    const QJsonObject &manifest,
    QString &error)
{
    const QString journalPath =
        fixture.temporary.filePath(
            QStringLiteral(
                ".gc-transactions/split-activity/%1")
                .arg(id));
    if (!QDir().mkpath(journalPath)) {
        error = QStringLiteral(
            "cannot create hostile journal");
        return false;
    }
    return writeBytes(
        QDir(journalPath).filePath(
            QStringLiteral("manifest.json")),
        QJsonDocument(manifest)
                .toJson(QJsonDocument::Compact)
            + '\n',
        error);
}

QPair<int, QString> runCrashChild(
    const QString &testFunction,
    const QString &dataTag,
    const QString &root,
    const QString &mode,
    const QString &phase = QString(),
    int occurrence = 1,
    bool keepOriginal = false)
{
    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QString::fromLatin1(CrashRootEnvironment), root);
    environment.insert(
        QString::fromLatin1(CrashModeEnvironment), mode);
    environment.insert(
        QString::fromLatin1(CrashKeepOriginalEnvironment),
        keepOriginal ? QStringLiteral("1")
                     : QStringLiteral("0"));
    if (!phase.isEmpty()) {
        environment.insert(
            QString::fromLatin1(CrashPhaseEnvironment),
            phase);
        environment.insert(
            QString::fromLatin1(CrashOccurrenceEnvironment),
            QString::number(occurrence));
    } else {
        environment.remove(
            QString::fromLatin1(CrashPhaseEnvironment));
        environment.remove(
            QString::fromLatin1(CrashOccurrenceEnvironment));
    }
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    const QString selector = dataTag.isEmpty()
        ? testFunction
        : QStringLiteral("%1:%2").arg(testFunction, dataTag);
    child.start(
        QCoreApplication::applicationFilePath(),
        {selector});
    if (!child.waitForStarted(5000)) {
        return qMakePair(-1, child.errorString());
    }
    if (!child.waitForFinished(30000)) {
        child.kill();
        child.waitForFinished();
        return qMakePair(
            -2, QStringLiteral("child timed out"));
    }
    return qMakePair(
        child.exitCode(),
        QString::fromUtf8(child.readAll()));
}

void crashAtRequestedTransition(const QByteArray &current)
{
    const QByteArray requested =
        qgetenv(CrashPhaseEnvironment);
    if (requested.isEmpty() || requested != current) return;
    static QHash<QByteArray, int> occurrences;
    const int occurrence = ++occurrences[requested];
    bool valid = false;
    const int requestedOccurrence =
        qEnvironmentVariableIntValue(
            CrashOccurrenceEnvironment, &valid);
    if (occurrence
        == (valid ? requestedOccurrence : 1)) {
        std::_Exit(SplitCrashExitCode);
    }
}

} // namespace

void splitActivitySaveDurableTransitionReached(
    const char *transition)
{
    const QByteArray current(transition);
    if (splitInjection
            == SplitInjection::CreateTargetBeforePublish
        && current == "split-before-output-published"
        && !injectionSucceeded) {
        QString error;
        injectionSucceeded = writeBytes(
            injectedTargetPath,
            injectedTargetContents,
            error);
    }
    if (splitInjection
            == SplitInjection::ReplacePublishedTargetAndFailSync
        && current == "split-output-published"
        && !injectionSucceeded) {
        const bool removed =
            QFile::remove(injectedTargetPath);
        QString error;
        injectionSucceeded = removed
            && writeBytes(
                injectedTargetPath,
                injectedTargetContents,
                error);
    }
    if ((splitInjection
                == SplitInjection::FailExplicitSync
            || splitInjection
                == SplitInjection::
                    ReplacePublishedTargetAndFailSync)
        && current
            == "split-before-output-parents-synchronized") {
        explicitSyncReached = true;
        failNextAnchoredSync = true;
    }
    if (splitInjection == SplitInjection::LeaveCommittedJournal
        && current == "split-committed-marker-published"
        && injectedJournalBlocker.isEmpty()) {
        const QDir nameSpace(
            QDir(injectedTransactionRoot).filePath(
                QStringLiteral(
                    ".gc-transactions/split-activity")));
        const QFileInfoList journals = nameSpace.entryInfoList(
            QDir::Dirs | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot);
        if (journals.size() == 1) {
            injectedJournalBlocker =
                QDir(journals.constFirst().absoluteFilePath())
                    .filePath(QStringLiteral("unexpected.blocker"));
            QString blockerError;
            injectionSucceeded = writeBytes(
                injectedJournalBlocker,
                QByteArray("block cleanup"),
                blockerError);
        }
    }

    crashAtRequestedTransition(current);
}

bool splitActivitySaveMoveAllowed(
    const QString &sourcePath,
    const QString &destinationPath)
{
    return !requireSameParentMoves
        || atomicFilePathKey(QFileInfo(sourcePath).absolutePath())
            == atomicFilePathKey(
                QFileInfo(destinationPath).absolutePath());
}

void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &sourcePath,
    const QString &)
{
    if (QByteArray(transition)
            == QByteArrayLiteral("directory-enumeration-first-pass")) {
        enumeratedDirectories.append(sourcePath);
    }
    crashAtRequestedTransition(QByteArray(transition));
}

bool anchoredFilesystemSyncFailureRequested(
    const QString &path)
{
    if (!failNextAnchoredSync
        || atomicFilePathKey(path)
            != atomicFilePathKey(injectedSyncParent)) {
        return false;
    }
    failNextAnchoredSync = false;
    anchoredSyncFailureConsumed = true;
    return true;
}

bool anchoredFilesystemFileUnlinkFailureRequested(
    const QString &path)
{
    if (injectedUnlinkFailuresRemaining > 0
        && path.contains(injectedUnlinkPathFragment)) {
        --injectedUnlinkFailuresRemaining;
        return true;
    }
    return false;
}

bool anchoredFilesystemUseLegacyWindowsDelete()
{
    return false;
}

void anchoredFilesystemOpenStateChanged(int delta)
{
    liveAnchoredStates += delta;
    peakAnchoredStates = std::max(
        peakAnchoredStates, liveAnchoredStates);
}

bool anchoredFilesystemDurableGenerationUnavailableRequested()
{
    return forceGenerationUnavailable;
}

qint64 splitActivitySaveRecoveryByteLimitForTest()
{
    return recoveryByteLimit;
}

qint64 splitActivitySaveRecoveryOperationLimitForTest()
{
    return recoveryOperationLimit;
}

qint64 splitActivitySaveRecoveryDeadlineForTest()
{
    return recoveryDeadlineMilliseconds;
}

qint64 splitActivitySaveRecoveryElapsedMillisecondsForTest()
{
    if (recoveryDeadlineAfterBytes < 0
        || recoveryDeadlineMilliseconds < 0) {
        return -1;
    }
    return recoveryBytesConsumed >= recoveryDeadlineAfterBytes
        ? recoveryDeadlineMilliseconds + 1
        : 0;
}

void splitActivitySaveRecoveryBudgetStarted()
{
    ++recoveryBudgetStarts;
}

void splitActivitySaveRecoveryBytesConsumed(qint64 bytes)
{
    recoveryBytesConsumed += bytes;
}

void splitActivitySavePathValidationStep()
{
    ++pathValidationSteps;
}

class TestSplitActivitySave : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void emptyOutputsAreRejected();
    void unsafeAndDuplicateOutputsAreRejected();
    void existingTargetIsPreserved();
    void targetSymlinkIsPreserved();
    void stagingFailureUsesDurableCleanup();
    void sourceChangeDuringStagingIsRejected();
    void keepOriginalSuccess();
    void removeOriginalSuccess();
    void customCallbacksCannotBypassJournal();
    void stagingCallbackHasNoPathAuthority();
    void preManifestQuarantineIsRecovered();
    void interruptedJournalTemporaryIsRecovered();
    void intentOnlyForeignArtifactIsPreserved();
    void committedRecoveryPreservesLaterEditsAndReusedSourceIdentity();
    void maximumAcceptedTransactionKeepsHandlesBounded();
    void preparedRollbackKeepsHandlesBounded();
    void intentValidationKeepsAnchoredHandlesBounded();
    void committingRecoveryPreservesRecreatedSourceIdentity();
    void committingRecoveryPreservesRecreatedBackupIdentity();
    void unavailableGenerationFailsBeforeJournalPublication();
    void keepOriginalRequiresDurableStageGeneration();
    void replacedJournalPayloadIsPreserved_data();
    void replacedJournalPayloadIsPreserved();
    void largeQuarantinedJournalPayloadIsRecovered_data();
    void largeQuarantinedJournalPayloadIsRecovered();
    void committingRecoveryFitsBoundedReadBudget();
    void recoveryDeadlineStopsMidPayloadAndResumes_data();
    void recoveryDeadlineStopsMidPayloadAndResumes();
    void maximumManifestPathValidationIsBounded();
    void recoveryBudgetIsSharedAcrossJournalsAndPhases();
    void largeAndDeepTreesDoNotBlockRecovery();
    void publicationMovesStayWithinTargetFilesystem();
    void outputLimitIsEnforced();
    void intermediateSymlinkEscapeIsRejected();
    void caseDistinctJournalNamesDoNotRequireRootScan();
    void targetAppearingAfterPreflightIsPreserved();
    void foreignSameContentTargetIsPreserved();
    void forwardOnlyRecoveryNeverDeletesReusedOutputIdentity();
    void keepOriginalParentSyncFailureRollsBack();
    void hostileManifestIsRejected_data();
    void hostileManifestIsRejected();
    void crashDuringOwnershipRecordPreservesArtifact();
    void crashRecoveryAtDurableTransition_data();
    void crashRecoveryAtDurableTransition();
};

void TestSplitActivitySave::cleanup()
{
    resetInjection();
}

void TestSplitActivitySave::emptyOutputsAreRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    QStringList published{QStringLiteral("stale.json")};
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {},
        true,
        published,
        error));
    QVERIFY(!error.isEmpty());
    QVERIFY(published.isEmpty());
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
}

void TestSplitActivitySave::
unsafeAndDuplicateOutputsAreRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            QStringLiteral("../escape.json"),
            QByteArray("unsafe"),
            &stageCalls)},
        true,
        published,
        error));
    QCOMPARE(stageCalls, 0);

    error.clear();
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {
            outputWriting(
                firstOutputName(),
                QByteArray("one"),
                &stageCalls),
            outputWriting(
                firstOutputName(),
                QByteArray("two"),
                &stageCalls)
        },
        true,
        published,
        error));
    QCOMPARE(stageCalls, 0);
    QVERIFY(!error.isEmpty());
    QVERIFY(!filePresent(
        fixture.activities.filePath(
            firstOutputName())));
}

void TestSplitActivitySave::existingTargetIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QString target =
        fixture.activities.filePath(firstOutputName());
    const QByteArray existing("existing target");
    writeFixture(target, existing);

    int stageCalls = 0;
    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("replacement"),
            &stageCalls)},
        true,
        published,
        error));
    QCOMPARE(stageCalls, 0);
    QCOMPARE(readBytes(target), existing);
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
}

void TestSplitActivitySave::targetSymlinkIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QString outside =
        fixture.temporary.filePath(
            QStringLiteral("outside.json"));
    const QByteArray outsideContents("outside");
    writeFixture(outside, outsideContents);
    const QString target =
        fixture.activities.filePath(firstOutputName());
    if (!QFile::link(outside, target)
        || !QFileInfo(target).isSymLink()) {
        QSKIP(
            "File symbolic links are unavailable");
    }

    int stageCalls = 0;
    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("replacement"),
            &stageCalls)},
        true,
        published,
        error));
    QCOMPARE(stageCalls, 0);
    QVERIFY(QFileInfo(target).isSymLink());
    QCOMPARE(readBytes(outside), outsideContents);
}

void TestSplitActivitySave::
stagingFailureUsesDurableCleanup()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    SplitActivityOutput failure;
    failure.fileName = secondOutputName();
    failure.stage =
        [&stageCalls](QByteArray &, QString &error) {
            ++stageCalls;
            error = QStringLiteral(
                "injected staging failure");
            return false;
        };

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {
            outputWriting(
                firstOutputName(),
                QByteArray("one"),
                &stageCalls),
            failure
        },
        false,
        published,
        error));
    QCOMPARE(stageCalls, 2);
    QVERIFY(error.contains(
        QStringLiteral("injected staging failure")));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
    QVERIFY(!filePresent(fixture.backupPath));
    QVERIFY(!filePresent(
        fixture.activities.filePath(
            firstOutputName())));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::
sourceChangeDuringStagingIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray original("source-version-one");
    const QByteArray changed("source-version-two");
    QCOMPARE(original.size(), changed.size());
    writeFixture(fixture.sourcePath, original);

    SplitActivityOutput output;
    output.fileName = firstOutputName();
    output.stage = [&](QByteArray &staged, QString &error) {
        staged = QByteArray("output");
        return writeBytes(fixture.sourcePath, changed, error);
    };

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {output},
        true,
        published,
        error));
    QCOMPARE(readBytes(fixture.sourcePath), changed);
    QVERIFY(!filePresent(
        fixture.activities.filePath(firstOutputName())));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::keepOriginalSuccess()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    QStringList published;
    QString error;
    QVERIFY2(
        saveSplitActivityFiles(
            fixture.activities,
            fixture.sourcePath,
            fixture.backupPath,
            {
                outputWriting(
                    firstOutputName(),
                    QByteArray("one")),
                outputWriting(
                    secondOutputName(),
                    QByteArray("two"))
            },
            true,
            published,
            error),
        qPrintable(error));
    QCOMPARE(
        published,
        QStringList(
            {firstOutputName(), secondOutputName()}));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
    QCOMPARE(
        readBytes(
            fixture.activities.filePath(
                firstOutputName())),
        QByteArray("one"));
    QCOMPARE(
        readBytes(
            fixture.activities.filePath(
                secondOutputName())),
        QByteArray("two"));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::removeOriginalSuccess()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    writeFixture(
        fixture.backupPath,
        QByteArray("previous backup"));

    QStringList published;
    QString error;
    QVERIFY2(
        saveSplitActivityFiles(
            fixture.activities,
            fixture.sourcePath,
            fixture.backupPath,
            {outputWriting(
                firstOutputName(),
                QByteArray("output"))},
            false,
            published,
            error),
        qPrintable(error));
    QVERIFY(!filePresent(fixture.sourcePath));
    QCOMPARE(
        readBytes(fixture.backupPath),
        sourceContents());
    QCOMPARE(
        readBytes(
            fixture.activities.filePath(
                firstOutputName())),
        QByteArray("output"));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::
customCallbacksCannotBypassJournal()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    const AtomicPublishFunction publish =
        [&publishCalls](
            const QString &,
            const QString &,
            bool &,
            QString &) {
            ++publishCalls;
            return true;
        };
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](
            const QString &,
            const QString &,
            QString &) {
            ++archiveCalls;
            return true;
        };

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("output"),
            &stageCalls)},
        false,
        published,
        error,
        publish));
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(error.contains(
        QStringLiteral("cannot bypass"),
        Qt::CaseInsensitive));

    error.clear();
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("output"),
            &stageCalls)},
        false,
        published,
        error,
        AtomicPublishFunction(),
        archive));
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(error.contains(
        QStringLiteral("cannot bypass"),
        Qt::CaseInsensitive));
}

void TestSplitActivitySave::stagingCallbackHasNoPathAuthority()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    bool callbackCalled = false;
    SplitActivityOutput output;
    output.fileName = firstOutputName();
    output.stage = [&callbackCalled](
            QByteArray &contents, QString &error) {
        Q_UNUSED(error)
        callbackCalled = true;
        contents = QByteArray("anchored output");
        return true;
    };

    QStringList published;
    QString error;
    QVERIFY2(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {output},
        true,
        published,
        error), qPrintable(error));
    QVERIFY(callbackCalled);
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        QByteArray("anchored output"));
}

void TestSplitActivitySave::preManifestQuarantineIsRecovered()
{
#ifndef Q_OS_UNIX
    QSKIP("File-removal quarantines are Unix-specific");
#endif
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    SplitActivityOutput failure;
    failure.fileName = secondOutputName();
    failure.stage = [](QByteArray &, QString &error) {
        error = QStringLiteral("injected staging failure");
        return false;
    };
    injectedUnlinkPathFragment = QStringLiteral(".gc-remove-");
    injectedUnlinkFailuresRemaining = 1;

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {
            outputWriting(firstOutputName(), QByteArray("first")),
            failure
        },
        false,
        published,
        error));
    QCOMPARE(injectedUnlinkFailuresRemaining, 0);
    QVERIFY(!splitProductionArtifactsAreAbsent(fixture));

    injectedUnlinkPathFragment.clear();
    enumeratedDirectories.clear();
    QString recoveryError;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError),
        qPrintable(recoveryError));
    QString secondRecoveryError;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), secondRecoveryError),
        qPrintable(secondRecoveryError));
    QVERIFY(splitProductionArtifactsAreAbsent(fixture));
    QVERIFY(splitNamespaceIsEmpty(fixture));
    QVERIFY(!enumeratedDirectories.contains(
        QDir::cleanPath(fixture.activities.absolutePath())));
}

void TestSplitActivitySave::interruptedJournalTemporaryIsRecovered()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString journalPath = QDir(fixture.transactionNamespace())
        .filePath(id);
    QVERIFY(QDir().mkpath(journalPath));
    writeFixture(
        QDir(journalPath).filePath(
            QStringLiteral("intent.json.tmp")),
        QByteArray("{torn"));

    QString error;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error), qPrintable(error));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::intentOnlyForeignArtifactIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    QString setupError;
    QVERIFY2(createIntentOnlyJournal(fixture, setupError),
        qPrintable(setupError));
    const QString journalPath = onlyJournalPath(fixture);
    QVERIFY(!journalPath.isEmpty());
    const QString id = QFileInfo(journalPath).fileName();
    const QString artifact = fixture.activities.filePath(
        QStringLiteral(".gc-split-%1-output-0000.stage").arg(id));
    const QByteArray foreign("foreign recovery data");
    writeFixture(artifact, foreign);

    QString error;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error));
    QVERIFY(error.contains(
        QStringLiteral("unproven"), Qt::CaseInsensitive));
    QVERIFY(error.contains(artifact));
    QCOMPARE(readBytes(artifact), foreign);

    QVERIFY(QFile::remove(artifact));
    error.clear();
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error), qPrintable(error));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::
committedRecoveryPreservesLaterEditsAndReusedSourceIdentity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    resetInjection();
    splitInjection = SplitInjection::LeaveCommittedJournal;
    injectedTransactionRoot = fixture.temporary.path();
    QStringList published;
    QString error;
    QVERIFY(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(firstOutputName(), QByteArray("committed"))},
        false,
        published,
        error));
    QVERIFY(injectionSucceeded);
    QVERIFY(!error.isEmpty());
    QVERIFY(filePresent(injectedJournalBlocker));
    QVERIFY(!filePresent(fixture.sourcePath));

    const QString outputPath =
        fixture.activities.filePath(firstOutputName());
    const QByteArray laterEdit("legitimate later edit");
    writeFixture(outputPath, laterEdit);
    // Recreate the source with identical bytes and make the journal look as
    // though the native identity was reused by the filesystem.
    const QByteArray laterSource = sourceContents();
    writeFixture(fixture.sourcePath, laterSource);
    const QByteArray laterBackup("legitimate backup edit");
    writeFixture(fixture.backupPath, laterBackup);
    QString identityError;
    QVERIFY2(replaceManifestSourceIdentity(
        fixture, identityError), qPrintable(identityError));
    QVERIFY(QFile::remove(injectedJournalBlocker));

    QString recoveryError;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError),
        qPrintable(recoveryError));
    QCOMPARE(readBytes(outputPath), laterEdit);
    QCOMPARE(readBytes(fixture.sourcePath), laterSource);
    QCOMPARE(readBytes(fixture.backupPath), laterBackup);
    QVERIFY(splitNamespaceIsEmpty(fixture));
    resetInjection();
}

void TestSplitActivitySave::
maximumAcceptedTransactionKeepsHandlesBounded()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    QList<SplitActivityOutput> outputs;
    outputs.reserve(1000);
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026_01_01_00_00_00"),
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
    for (int index = 0; index < 1000; ++index) {
        outputs.append(outputWriting(
            timestamp.addSecs(index).toString(
                QStringLiteral("yyyy_MM_dd_HH_mm_ss'.json'")),
            QByteArray("x")));
    }

    liveAnchoredStates = 0;
    peakAnchoredStates = 0;
    QStringList published;
    QString error;
    QVERIFY2(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        true,
        published,
        error), qPrintable(error));
    QCOMPARE(published.size(), 1000);
    QVERIFY2(peakAnchoredStates <= 16,
        qPrintable(QStringLiteral("peak anchored states: %1")
            .arg(peakAnchoredStates)));
    QCOMPARE(liveAnchoredStates, 0);
}

void TestSplitActivitySave::
preparedRollbackKeepsHandlesBounded()
{
    const QString childRoot =
        qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode =
        qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("bounded-rollback-crash")) {
        const QDir activities(
            QDir(childRoot).filePath(QStringLiteral("activities")));
        QList<SplitActivityOutput> outputs;
        const QDateTime timestamp = QDateTime::fromString(
            QStringLiteral("2026_01_02_00_00_00"),
            QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
        for (int index = 0; index < 64; ++index) {
            outputs.append(outputWriting(
                timestamp.addSecs(index).toString(
                    QStringLiteral("yyyy_MM_dd_HH_mm_ss'.json'")),
                QByteArray("x")));
        }
        QStringList published;
        QString error;
        saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            QDir(childRoot).filePath(
                QStringLiteral("backup/original-activity.json.bak")),
            outputs,
            false,
            published,
            error);
        QFAIL(qPrintable(error));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    writeFixture(fixture.backupPath, QByteArray("prior backup"));
    const auto crashed = runCrashChild(
        QStringLiteral("preparedRollbackKeepsHandlesBounded"),
        QString(),
        fixture.temporary.path(),
        QStringLiteral("bounded-rollback-crash"),
        QStringLiteral("split-prepared-manifest-published"));
    QCOMPARE(crashed.first, SplitCrashExitCode);

    liveAnchoredStates = 0;
    peakAnchoredStates = 0;
    QString error;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error), qPrintable(error));
    QVERIFY2(peakAnchoredStates <= 16,
        qPrintable(QStringLiteral("peak anchored states: %1")
            .arg(peakAnchoredStates)));
    QCOMPARE(liveAnchoredStates, 0);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), QByteArray("prior backup"));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::
intentValidationKeepsAnchoredHandlesBounded()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    QString setupError;
    QVERIFY2(createIntentOnlyJournal(
        fixture, setupError, 64, true), qPrintable(setupError));

    liveAnchoredStates = 0;
    peakAnchoredStates = 0;
    QString error;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error), qPrintable(error));
    QVERIFY2(peakAnchoredStates <= 16,
        qPrintable(QStringLiteral("peak anchored states: %1")
            .arg(peakAnchoredStates)));
    QCOMPARE(liveAnchoredStates, 0);
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::
committingRecoveryPreservesRecreatedSourceIdentity()
{
    const QString childRoot =
        qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode =
        qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("source-reuse-crash")) {
        const QDir activities(
            QDir(childRoot).filePath(QStringLiteral("activities")));
        QStringList published;
        QString error;
        saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            QDir(childRoot).filePath(
                QStringLiteral("backup/original-activity.json.bak")),
            {outputWriting(firstOutputName(), QByteArray("output"))},
            false,
            published,
            error);
        QFAIL(qPrintable(error));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    writeFixture(fixture.backupPath, QByteArray("prior backup"));
    const auto crashed = runCrashChild(
        QStringLiteral(
            "committingRecoveryPreservesRecreatedSourceIdentity"),
        QString(),
        fixture.temporary.path(),
        QStringLiteral("source-reuse-crash"),
        QStringLiteral("split-committing-marker-published"));
    QCOMPARE(crashed.first, SplitCrashExitCode);

    QVERIFY(QFile::remove(fixture.sourcePath));
    writeFixture(fixture.sourcePath, sourceContents());
    QString setupError;
    QVERIFY2(replaceManifestSourceIdentity(fixture, setupError),
        qPrintable(setupError));

    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY2(recoveryError.contains(
        QStringLiteral("generation"), Qt::CaseInsensitive),
        qPrintable(recoveryError));
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
}

void TestSplitActivitySave::
committingRecoveryPreservesRecreatedBackupIdentity()
{
    const QString childRoot =
        qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode =
        qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("backup-reuse-crash")) {
        const QDir activities(
            QDir(childRoot).filePath(QStringLiteral("activities")));
        QStringList published;
        QString error;
        saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            QDir(childRoot).filePath(
                QStringLiteral("backup/original-activity.json.bak")),
            {outputWriting(firstOutputName(), QByteArray("output"))},
            false,
            published,
            error);
        QFAIL(qPrintable(error));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray priorBackup("prior backup");
    writeFixture(fixture.backupPath, priorBackup);
    const auto crashed = runCrashChild(
        QStringLiteral(
            "committingRecoveryPreservesRecreatedBackupIdentity"),
        QString(),
        fixture.temporary.path(),
        QStringLiteral("backup-reuse-crash"),
        QStringLiteral("split-committing-marker-published"));
    QCOMPARE(crashed.first, SplitCrashExitCode);

    QVERIFY(QFile::remove(fixture.backupPath));
    writeFixture(fixture.backupPath, priorBackup);
    QString setupError;
    QVERIFY2(replaceManifestFileIdentity(
        fixture,
        QStringLiteral("backup"),
        fixture.backupPath,
        setupError), qPrintable(setupError));

    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY2(recoveryError.contains(
        QStringLiteral("generation"), Qt::CaseInsensitive),
        qPrintable(recoveryError));
    QCOMPARE(readBytes(fixture.backupPath), priorBackup);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
}

void TestSplitActivitySave::
unavailableGenerationFailsBeforeJournalPublication()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray priorBackup("prior backup");
    writeFixture(fixture.backupPath, priorBackup);

    forceGenerationUnavailable = true;
    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(firstOutputName(), QByteArray("output"))},
        false,
        published,
        error));
    QVERIFY2(error.contains(
        QStringLiteral("generation"), Qt::CaseInsensitive),
        qPrintable(error));
    QVERIFY(onlyJournalPath(fixture).isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), priorBackup);
}

void TestSplitActivitySave::
keepOriginalRequiresDurableStageGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    forceGenerationUnavailable = true;
    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(firstOutputName(), QByteArray("output"))},
        true,
        published,
        error));
    QVERIFY2(error.contains(
        QStringLiteral("generation"), Qt::CaseInsensitive),
        qPrintable(error));
    QVERIFY(published.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QVERIFY(!filePresent(
        fixture.activities.filePath(firstOutputName())));
    QVERIFY(splitNamespaceIsEmpty(fixture));
    QVERIFY(splitProductionArtifactsAreAbsent(fixture));
}

void TestSplitActivitySave::replacedJournalPayloadIsPreserved_data()
{
    QTest::addColumn<QString>("payloadName");
    QTest::addColumn<bool>("keepOriginal");
    QTest::addColumn<QString>("mutation");

    QTest::newRow("source-identity")
        << QStringLiteral("source.old") << false
        << QStringLiteral("identity");
    QTest::newRow("backup-identity")
        << QStringLiteral("backup.old") << false
        << QStringLiteral("identity");
    QTest::newRow("output-identity")
        << QStringLiteral("output-0000.new") << true
        << QStringLiteral("identity");
    QTest::newRow("output-size")
        << QStringLiteral("output-0000.new") << true
        << QStringLiteral("size");
    QTest::newRow("output-digest")
        << QStringLiteral("output-0000.new") << true
        << QStringLiteral("digest");
    QTest::newRow("output-generation")
        << QStringLiteral("output-0000.new") << true
        << QStringLiteral("generation");
}

void TestSplitActivitySave::replacedJournalPayloadIsPreserved()
{
    QFETCH(QString, payloadName);
    QFETCH(bool, keepOriginal);
    QFETCH(QString, mutation);

    const QString childRoot = qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode = qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("payload-binding-crash")) {
        const QDir root(childRoot);
        const QDir activities(root.filePath(QStringLiteral("activities")));
        QStringList published;
        QString error;
        saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            root.filePath(
                QStringLiteral("backup/original-activity.json.bak")),
            {outputWriting(firstOutputName(), QByteArray("output"))},
            qEnvironmentVariableIntValue(
                CrashKeepOriginalEnvironment) != 0,
            published,
            error);
        QFAIL(qPrintable(error));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray priorBackup("prior backup");
    writeFixture(fixture.backupPath, priorBackup);
    const QString dataTag = QString::fromLatin1(QTest::currentDataTag());
    const auto crashed = runCrashChild(
        QStringLiteral("replacedJournalPayloadIsPreserved"),
        dataTag,
        fixture.temporary.path(),
        QStringLiteral("payload-binding-crash"),
        QStringLiteral("split-prepared-manifest-published"),
        1,
        keepOriginal);
    QCOMPARE(crashed.first, SplitCrashExitCode);

    const QByteArray foreign = mutation == QStringLiteral("digest")
        ? QByteArray("tamper")
        : (mutation == QStringLiteral("generation")
            ? QByteArray("output")
            : QByteArray("foreign journal payload"));
    QString setupError;
    if (mutation == QStringLiteral("generation")) {
        QVERIFY2(alterManifestOutputStoredGeneration(
            fixture, setupError), qPrintable(setupError));
    } else if (mutation == QStringLiteral("identity")) {
        QVERIFY2(replaceJournalPayload(
            fixture, payloadName, foreign, setupError),
            qPrintable(setupError));
    } else {
        QVERIFY2(writeBytes(
            QDir(onlyJournalPath(fixture)).filePath(payloadName),
            foreign,
            setupError), qPrintable(setupError));
    }

    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY2(recoveryError.contains(
        QStringLiteral("payload"), Qt::CaseInsensitive),
        qPrintable(recoveryError));
    QCOMPARE(
        readBytes(QDir(onlyJournalPath(fixture)).filePath(payloadName)),
        foreign);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), priorBackup);
    QVERIFY(!filePresent(
        fixture.activities.filePath(firstOutputName())));
}

void TestSplitActivitySave::
largeQuarantinedJournalPayloadIsRecovered_data()
{
    QTest::addColumn<QString>("payloadName");
    QTest::newRow("source") << QStringLiteral("source.old");
    QTest::newRow("output") << QStringLiteral("output-0000.new");
}

void TestSplitActivitySave::
largeQuarantinedJournalPayloadIsRecovered()
{
#ifndef Q_OS_UNIX
    QSKIP("Removal quarantines are Unix-specific");
#endif
    QFETCH(QString, payloadName);

    constexpr int PayloadSize = 96 * 1024;
    const QByteArray largeSource(PayloadSize, 's');
    const QByteArray largeOutput(PayloadSize, 'o');
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(
        fixture.sourcePath,
        payloadName == QStringLiteral("source.old")
            ? largeSource : sourceContents());

    splitInjection = SplitInjection::LeaveCommittedJournal;
    injectedTransactionRoot = fixture.temporary.path();
    QStringList published;
    QString saveError;
    QVERIFY2(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            payloadName == QStringLiteral("output-0000.new")
                ? largeOutput : QByteArray("output"))},
        true,
        published,
        saveError), qPrintable(saveError));
    QVERIFY(injectionSucceeded);

    const QString journalPath = onlyJournalPath(fixture);
    QVERIFY(!journalPath.isEmpty());
    QVERIFY(QFile::remove(injectedJournalBlocker));
    splitInjection = SplitInjection::None;

    QString anchorError;
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    QVERIFY2(AnchoredFileSystem::DirectoryAnchor::open(
        journalPath, journalDirectory, anchorError),
        qPrintable(anchorError));
    const AnchoredFileSystem::EntryRef payloadEntry =
        journalDirectory.entry(payloadName, anchorError);
    QVERIFY2(payloadEntry.isValid(), qPrintable(anchorError));
    AnchoredFileSystem::PinnedFile payload;
    QVERIFY2(AnchoredFileSystem::pinRegularFile(
        payloadEntry, payload, anchorError, PayloadSize),
        qPrintable(anchorError));
    const QString quarantineName =
        AnchoredFileSystem::removalQuarantineName(
            payload.identity(), payloadName);
    QVERIFY(!quarantineName.isEmpty());
    const QString payloadPath = QDir(journalPath).filePath(payloadName);
    const QString quarantinePath =
        QDir(journalPath).filePath(quarantineName);
    payload = {};
    QVERIFY(QFile::rename(payloadPath, quarantinePath));

    QString recoveryError;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError),
        qPrintable(recoveryError));
    QVERIFY(splitNamespaceIsEmpty(fixture));
    QVERIFY(!filePresent(quarantinePath));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        payloadName == QStringLiteral("source.old")
            ? largeSource : sourceContents());
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        payloadName == QStringLiteral("output-0000.new")
            ? largeOutput : QByteArray("output"));
}

void TestSplitActivitySave::committingRecoveryFitsBoundedReadBudget()
{
    constexpr int PayloadSize = 2 * 1024 * 1024;
    const QString childRoot = qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode = qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("read-budget-crash")) {
        const QDir root(childRoot);
        const QDir activities(root.filePath(QStringLiteral("activities")));
        QStringList published;
        QString error;
        saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            root.filePath(
                QStringLiteral("backup/original-activity.json.bak")),
            {outputWriting(firstOutputName(), QByteArray(PayloadSize, 'o'))},
            false,
            published,
            error);
        QFAIL(qPrintable(error));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, QByteArray(PayloadSize, 's'));
    writeFixture(fixture.backupPath, QByteArray(PayloadSize, 'b'));
    const auto crashed = runCrashChild(
        QStringLiteral("committingRecoveryFitsBoundedReadBudget"),
        QString(),
        fixture.temporary.path(),
        QStringLiteral("read-budget-crash"),
        QStringLiteral("split-committing-marker-published"));
    QCOMPARE(crashed.first, SplitCrashExitCode);

    const qint64 aggregate = 3LL * PayloadSize;
#ifdef Q_OS_UNIX
    recoveryByteLimit = 8 * aggregate + 1024 * 1024;
    const qint64 minimumExpectedReads = 7 * aggregate;
#else
    recoveryByteLimit = 6 * aggregate + 1024 * 1024;
    const qint64 minimumExpectedReads = 5 * aggregate;
#endif
    recoveryBytesConsumed = 0;
    QString error;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error), qPrintable(error));
    QVERIFY(recoveryBytesConsumed > 0);
    QVERIFY(recoveryBytesConsumed > minimumExpectedReads);
    QVERIFY(recoveryBytesConsumed <= recoveryByteLimit);
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        QByteArray(PayloadSize, 'o'));
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::
recoveryDeadlineStopsMidPayloadAndResumes_data()
{
    QTest::addColumn<qint64>("deadlineAfterBytes");
    QTest::addColumn<bool>("outputPublished");
    QTest::newRow("pin") << qint64(512 * 1024) << false;
    QTest::newRow("move")
        << qint64(2 * 1024 * 1024 + 512 * 1024) << true;
}

void TestSplitActivitySave::
recoveryDeadlineStopsMidPayloadAndResumes()
{
    QFETCH(qint64, deadlineAfterBytes);
    QFETCH(bool, outputPublished);

    constexpr int PayloadSize = 2 * 1024 * 1024;
    const QString childRoot = qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode = qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("deadline-budget-crash")) {
        const QDir root(childRoot);
        const QDir activities(root.filePath(QStringLiteral("activities")));
        QStringList published;
        QString error;
        saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            root.filePath(
                QStringLiteral("backup/original-activity.json.bak")),
            {outputWriting(
                firstOutputName(), QByteArray(PayloadSize, 'o'))},
            false,
            published,
            error);
        QFAIL(qPrintable(error));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray source(PayloadSize, 's');
    const QByteArray output(PayloadSize, 'o');
    writeFixture(fixture.sourcePath, source);
    const auto crashed = runCrashChild(
        QStringLiteral("recoveryDeadlineStopsMidPayloadAndResumes"),
        QString::fromLatin1(QTest::currentDataTag()),
        fixture.temporary.path(),
        QStringLiteral("deadline-budget-crash"),
        QStringLiteral("split-committing-marker-published"));
    QCOMPARE(crashed.first, SplitCrashExitCode);

    recoveryDeadlineMilliseconds = 100;
    recoveryDeadlineAfterBytes = deadlineAfterBytes;
    recoveryBytesConsumed = 0;
    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY2(recoveryError.contains(
        QStringLiteral("elapsed-time"), Qt::CaseInsensitive),
        qPrintable(recoveryError));
    QVERIFY(!onlyJournalPath(fixture).isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), source);
    QCOMPARE(
        filePresent(fixture.activities.filePath(firstOutputName())),
        outputPublished);
    if (outputPublished) {
        QCOMPARE(
            readBytes(fixture.activities.filePath(firstOutputName())),
            output);
    }

    recoveryDeadlineMilliseconds = -1;
    recoveryDeadlineAfterBytes = -1;
    recoveryBytesConsumed = 0;
    recoveryError.clear();
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError),
        qPrintable(recoveryError));
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        output);
    QVERIFY(splitNamespaceIsEmpty(fixture));
}

void TestSplitActivitySave::maximumManifestPathValidationIsBounded()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, QByteArray("x"));
    const QString id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QJsonObject manifest = baseManifest(id);
    QJsonArray outputs;
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026_01_01_00_00_00"),
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
    for (int index = 0; index < 1000; ++index) {
        outputs.append(outputRecord(
            index,
            QStringLiteral("activities/%1")
                .arg(timestamp.addSecs(index).toString(
                    QStringLiteral("yyyy_MM_dd_HH_mm_ss'.json'"))),
            id));
    }
    manifest.insert(QStringLiteral("outputs"), outputs);
    QString setupError;
    QVERIFY2(createHostileJournal(
        fixture, id, manifest, setupError), qPrintable(setupError));

    pathValidationSteps = 0;
    QString error;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error));
    QVERIFY(pathValidationSteps > 0);
    QVERIFY2(pathValidationSteps <= 100000,
        qPrintable(QStringLiteral("path validation steps: %1")
            .arg(pathValidationSteps)));
}

void TestSplitActivitySave::
recoveryBudgetIsSharedAcrossJournalsAndPhases()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    for (int index = 0; index < 32; ++index) {
        QString setupError;
        QVERIFY2(createIntentOnlyJournal(fixture, setupError),
            qPrintable(setupError));
    }

    recoveryBudgetStarts = 0;
    QString error;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error), qPrintable(error));
    QCOMPARE(recoveryBudgetStarts, 1);

    for (int index = 0; index < 32; ++index) {
        QString setupError;
        QVERIFY2(createIntentOnlyJournal(fixture, setupError),
            qPrintable(setupError));
    }
    recoveryOperationLimit = 24;
    recoveryBudgetStarts = 0;
    error.clear();
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error));
    QVERIFY2(error.contains(
        QStringLiteral("budget"), Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(recoveryBudgetStarts, 1);

    recoveryOperationLimit = -1;
    Fixture phaseFixture;
    QVERIFY(phaseFixture.initialize());
    QString setupError;
    QVERIFY2(createIntentOnlyJournal(phaseFixture, setupError),
        qPrintable(setupError));
    const QString intentPath = QDir(onlyJournalPath(phaseFixture))
        .filePath(QStringLiteral("intent.json"));
    const qint64 intentSize = QFileInfo(intentPath).size();
    QVERIFY(intentSize > 0);
    recoveryByteLimit = intentSize * 3;
    recoveryBudgetStarts = 0;
    error.clear();
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        phaseFixture.temporary.path(), error));
    QVERIFY2(error.contains(
        QStringLiteral("budget"), Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(recoveryBudgetStarts, 1);
    QVERIFY(filePresent(intentPath));
}

void TestSplitActivitySave::largeAndDeepTreesDoNotBlockRecovery()
{
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        for (int index = 0; index <= 10000; ++index) {
            QVERIFY(QDir(fixture.temporary.path()).mkdir(
                QStringLiteral("unrelated-%1").arg(index)));
        }
        QString setupError;
        QVERIFY2(createIntentOnlyJournal(fixture, setupError),
            qPrintable(setupError));

        enumeratedDirectories.clear();
        QString recoveryError;
        QVERIFY2(SplitActivityTransaction::reconcileAll(
            fixture.temporary.path(), recoveryError),
            qPrintable(recoveryError));
        QVERIFY(splitNamespaceIsEmpty(fixture));
        QVERIFY(!enumeratedDirectories.contains(
            QDir::cleanPath(fixture.temporary.path())));
    }

    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        QString deep = fixture.temporary.filePath(
            QStringLiteral("unrelated"));
        for (int depth = 0; depth < 40; ++depth) {
            deep = QDir(deep).filePath(QStringLiteral("child"));
        }
        QVERIFY(QDir().mkpath(deep));
        QString setupError;
        QVERIFY2(createIntentOnlyJournal(fixture, setupError),
            qPrintable(setupError));

        QString recoveryError;
        QVERIFY2(SplitActivityTransaction::reconcileAll(
            fixture.temporary.path(), recoveryError),
            qPrintable(recoveryError));
        QVERIFY(splitNamespaceIsEmpty(fixture));
    }
}

void TestSplitActivitySave::
publicationMovesStayWithinTargetFilesystem()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray priorBackup("prior backup");
    writeFixture(fixture.backupPath, priorBackup);

    resetInjection();
    requireSameParentMoves = true;
    QStringList published;
    QString error;
    QVERIFY2(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(firstOutputName(), QByteArray("output"))},
        false,
        published,
        error), qPrintable(error));
    QVERIFY(!filePresent(fixture.sourcePath));
    QCOMPARE(readBytes(fixture.backupPath), sourceContents());
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        QByteArray("output"));
    resetInjection();
}

void TestSplitActivitySave::outputLimitIsEnforced()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    QList<SplitActivityOutput> outputs;
    outputs.reserve(1001);
    QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026_01_01_00_00_00"),
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
    for (int index = 0; index < 1001; ++index) {
        outputs.append(
            outputWriting(
                timestamp.addSecs(index).toString(
                    QStringLiteral(
                        "yyyy_MM_dd_HH_mm_ss'.json'")),
                QByteArray("x"),
                &stageCalls));
    }

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        true,
        published,
        error));
    QCOMPARE(stageCalls, 0);
    QVERIFY(error.contains(QStringLiteral("1000")));
    QVERIFY(!QFileInfo(
        fixture.temporary.filePath(
            QStringLiteral(".gc-transactions")))
                 .exists());
}

void TestSplitActivitySave::
intermediateSymlinkEscapeIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    const QString realDirectory =
        fixture.temporary.filePath(
            QStringLiteral("real-backup"));
    const QString alias =
        fixture.temporary.filePath(
            QStringLiteral("backup-alias"));
    QVERIFY(QDir().mkpath(realDirectory));
    if (!createDirectoryLink(realDirectory, alias)) {
        QSKIP(
            "Directory symlink or junction creation is unavailable");
    }

    int stageCalls = 0;
    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        QDir(alias).filePath(
            QStringLiteral("source.bak")),
        {outputWriting(
            firstOutputName(),
            QByteArray("output"),
            &stageCalls)},
        false,
        published,
        error));
    QCOMPARE(stageCalls, 0);
    QVERIFY(!filePresent(
        QDir(realDirectory).filePath(
            QStringLiteral("source.bak"))));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
}

void TestSplitActivitySave::
caseDistinctJournalNamesDoNotRequireRootScan()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    QVERIFY(QDir().mkpath(
        fixture.temporary.filePath(
            QStringLiteral(".GC-TRANSACTIONS"))));

    int stageCalls = 0;
    QStringList published;
    QString error;
    enumeratedDirectories.clear();
    QVERIFY2(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("output"),
            &stageCalls)},
        true,
        published,
        error), qPrintable(error));
    QCOMPARE(stageCalls, 1);
    QVERIFY(!enumeratedDirectories.contains(
        QDir::cleanPath(fixture.temporary.path())));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());

    Fixture namespaceFixture;
    QVERIFY(namespaceFixture.initialize());
    writeFixture(
        namespaceFixture.sourcePath,
        sourceContents());
    QVERIFY(QDir().mkpath(
        namespaceFixture.temporary.filePath(
            QStringLiteral(
                ".gc-transactions/SPLIT-ACTIVITY"))));
    stageCalls = 0;
    error.clear();
    enumeratedDirectories.clear();
    QVERIFY2(saveSplitActivityFiles(
        namespaceFixture.activities,
        namespaceFixture.sourcePath,
        namespaceFixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("output"),
            &stageCalls)},
        true,
        published,
        error), qPrintable(error));
    QCOMPARE(stageCalls, 1);
    QVERIFY(!enumeratedDirectories.contains(
        QDir::cleanPath(namespaceFixture.temporary.path())));
}

void TestSplitActivitySave::
targetAppearingAfterPreflightIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    resetInjection();
    splitInjection =
        SplitInjection::CreateTargetBeforePublish;
    injectedTargetPath =
        fixture.activities.filePath(firstOutputName());
    injectedTargetContents =
        QByteArray("concurrent target");

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("output"))},
        true,
        published,
        error));
    QVERIFY(injectionSucceeded);
    QCOMPARE(
        readBytes(injectedTargetPath),
        injectedTargetContents);
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
    resetInjection();
}

void TestSplitActivitySave::
foreignSameContentTargetIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    const QByteArray outputContents(
        "same contents, different inode");
    resetInjection();
    splitInjection =
        SplitInjection::
            ReplacePublishedTargetAndFailSync;
    injectedTargetPath =
        fixture.activities.filePath(firstOutputName());
    injectedTargetContents = outputContents;
    injectedSyncParent =
        fixture.activities.absolutePath();

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            outputContents)},
        true,
        published,
        error));
    QVERIFY(injectionSucceeded);
    QVERIFY(explicitSyncReached);
    QVERIFY(anchoredSyncFailureConsumed);
    const QString journalPath = onlyJournalPath(fixture);
    QVERIFY(!journalPath.isEmpty());
    QVERIFY(filePresent(
        QDir(journalPath).filePath(QStringLiteral("COMMITTING"))));
    QVERIFY(!filePresent(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
    QCOMPARE(
        readBytes(injectedTargetPath),
        outputContents);
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY(recoveryError.contains(
        QStringLiteral("foreign"), Qt::CaseInsensitive));
    QCOMPARE(readBytes(injectedTargetPath), outputContents);
    resetInjection();
}

void TestSplitActivitySave::
forwardOnlyRecoveryNeverDeletesReusedOutputIdentity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    const QByteArray outputContents(
        "same contents with a simulated reused identity");
    resetInjection();
    splitInjection =
        SplitInjection::ReplacePublishedTargetAndFailSync;
    injectedTargetPath =
        fixture.activities.filePath(firstOutputName());
    injectedTargetContents = outputContents;
    injectedSyncParent = fixture.activities.absolutePath();

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(firstOutputName(), outputContents)},
        true,
        published,
        error));
    QVERIFY(injectionSucceeded);
    const QString journalPath = onlyJournalPath(fixture);
    QVERIFY(!journalPath.isEmpty());
    QVERIFY(filePresent(
        QDir(journalPath).filePath(QStringLiteral("COMMITTING"))));
    QString identityError;
    QVERIFY2(replaceManifestOutputIdentity(
        fixture, firstOutputName(), identityError),
        qPrintable(identityError));

    splitInjection = SplitInjection::None;
    failNextAnchoredSync = false;
    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY(recoveryError.contains(
        QStringLiteral("generation"), Qt::CaseInsensitive));
    QCOMPARE(readBytes(injectedTargetPath), outputContents);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QVERIFY(!splitNamespaceIsEmpty(fixture));

    QString secondRecoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), secondRecoveryError));
    QVERIFY(secondRecoveryError.contains(
        QStringLiteral("generation"), Qt::CaseInsensitive));
    QCOMPARE(readBytes(injectedTargetPath), outputContents);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    resetInjection();
}

void TestSplitActivitySave::
keepOriginalParentSyncFailureRollsBack()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    resetInjection();
    splitInjection =
        SplitInjection::FailExplicitSync;
    injectedSyncParent =
        fixture.activities.absolutePath();

    QStringList published;
    QString error;
    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        {outputWriting(
            firstOutputName(),
            QByteArray("output"))},
        true,
        published,
        error));
    QVERIFY(explicitSyncReached);
    QVERIFY(anchoredSyncFailureConsumed);
    const QString outputPath = fixture.activities.filePath(
        firstOutputName());
    QCOMPARE(readBytes(outputPath), QByteArray("output"));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        sourceContents());
    splitInjection = SplitInjection::None;
    failNextAnchoredSync = false;
    QString recoveryError;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError),
        qPrintable(recoveryError));
    QCOMPARE(readBytes(outputPath), QByteArray("output"));
    QVERIFY(splitNamespaceIsEmpty(fixture));
    resetInjection();
}


void TestSplitActivitySave::
hostileManifestIsRejected_data()
{
    QTest::addColumn<QString>("attack");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("boolean-as-string")
        << QStringLiteral("boolean-as-string")
        << QStringLiteral("schema");
    QTest::newRow("fractional-schema")
        << QStringLiteral("fractional-schema")
        << QStringLiteral("invalid");
    QTest::newRow("null-uuid")
        << QStringLiteral("null-uuid")
        << QStringLiteral("unsafe entry");
    QTest::newRow("uppercase-uuid")
        << QStringLiteral("uppercase-uuid")
        << QStringLiteral("unsafe entry");
    QTest::newRow("mismatched-uuid")
        << QStringLiteral("mismatched-uuid")
        << QStringLiteral("invalid");
    QTest::newRow("source-output-collision")
        << QStringLiteral("source-output-collision")
        << QStringLiteral("colliding");
    QTest::newRow("journal-alias-path")
        << QStringLiteral("journal-alias-path")
        << QStringLiteral("malformed");
    QTest::newRow("identity-number")
        << QStringLiteral("identity-number")
        << QStringLiteral("type");
    QTest::newRow("extra-root-key")
        << QStringLiteral("extra-root-key")
        << QStringLiteral("schema");
    QTest::newRow("too-many-outputs")
        << QStringLiteral("too-many-outputs")
        << QStringLiteral("output set");
    QTest::newRow("oversized-payload")
        << QStringLiteral("oversized-payload")
        << QStringLiteral("content-read budget");
    QTest::newRow("aggregate-payload")
        << QStringLiteral("aggregate-payload")
        << QStringLiteral("content-read budget");
}

void TestSplitActivitySave::hostileManifestIsRejected()
{
    QFETCH(QString, attack);
    QFETCH(QString, expectedError);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, QByteArray("x"));

    QString id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    if (attack == QStringLiteral("null-uuid")) {
        id = QStringLiteral(
            "00000000-0000-0000-0000-000000000000");
    } else if (attack == QStringLiteral("uppercase-uuid")) {
        id = id.toUpper();
    }
    QJsonObject manifest = baseManifest(id);
    if (attack == QStringLiteral("mismatched-uuid")) {
        manifest.insert(
            QStringLiteral("id"),
            QUuid::createUuid()
                .toString(QUuid::WithoutBraces)
                .toLower());
    }
    if (attack == QStringLiteral("boolean-as-string")) {
        manifest.insert(
            QStringLiteral("keepOriginal"),
            QStringLiteral("true"));
    } else if (attack
        == QStringLiteral("fractional-schema")) {
        manifest.insert(
            QStringLiteral("schema"), 2.5);
    } else if (attack
        == QStringLiteral("source-output-collision")) {
        QJsonArray outputs;
        outputs.append(
            outputRecord(
                0,
                QStringLiteral(
                    "ACTIVITIES/2026_07_06_08_00_00.json"),
                id));
        manifest.insert(
            QStringLiteral("outputs"), outputs);
    } else if (attack
        == QStringLiteral("journal-alias-path")) {
        QJsonArray outputs;
        outputs.append(
            outputRecord(
                0,
                QStringLiteral(
                    ".GC-TRANSACTIONS/escape.json"),
                id));
        manifest.insert(
            QStringLiteral("outputs"), outputs);
    } else if (attack
        == QStringLiteral("identity-number")) {
        QJsonArray outputs =
            manifest.value(
                QStringLiteral("outputs")).toArray();
        QJsonObject output = outputs.at(0).toObject();
        output.insert(
            QStringLiteral("stored_identity"), 66);
        outputs.replace(0, output);
        manifest.insert(
            QStringLiteral("outputs"), outputs);
    } else if (attack
        == QStringLiteral("extra-root-key")) {
        manifest.insert(
            QStringLiteral("unexpected"), true);
    } else if (attack
        == QStringLiteral("too-many-outputs")) {
        QJsonArray outputs;
        for (int index = 0; index < 1001; ++index) {
            outputs.append(
                outputRecord(
                    index,
                    QStringLiteral(
                        "activities/hostile-%1.json")
                        .arg(index),
                    id));
        }
        manifest.insert(
            QStringLiteral("outputs"), outputs);
    } else if (attack
        == QStringLiteral("oversized-payload")) {
        QJsonObject source = manifest.value(
            QStringLiteral("source")).toObject();
        source.insert(
            QStringLiteral("size"),
            QString::number(256LL * 1024 * 1024 + 1));
        manifest.insert(QStringLiteral("source"), source);
    } else if (attack
        == QStringLiteral("aggregate-payload")) {
        QJsonArray outputs;
        for (int index = 0; index < 4; ++index) {
            QJsonObject output = outputRecord(
                index,
                QStringLiteral(
                    "activities/2026_07_06_08_30_%1.json")
                    .arg(index, 2, 10, QLatin1Char('0')),
                id);
            output.insert(
                QStringLiteral("size"),
                QString::number(256LL * 1024 * 1024));
            outputs.append(output);
        }
        manifest.insert(QStringLiteral("outputs"), outputs);
    }

    QString setupError;
    QVERIFY2(
        createHostileJournal(
            fixture, id, manifest, setupError),
        qPrintable(setupError));

    QString error;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), error));
    QVERIFY2(
        error.contains(
            expectedError, Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(
        readBytes(fixture.sourcePath),
        QByteArray("x"));
}

void TestSplitActivitySave::
crashDuringOwnershipRecordPreservesArtifact()
{
    const QString childRoot =
        qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode =
        qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty()
        && childMode == QStringLiteral("save-crash")) {
        const QDir root(childRoot);
        const QDir activities(
            root.filePath(QStringLiteral("activities")));
        QStringList published;
        QString error;
        const bool saved = saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            root.filePath(
                QStringLiteral(
                    "backup/original-activity.json.bak")),
            {
                outputWriting(
                    firstOutputName(),
                    QByteArray("first output")),
                outputWriting(
                    secondOutputName(),
                    QByteArray("second output"))
            },
            false,
            published,
            error);
        QFAIL(qPrintable(
            QStringLiteral(
                "Save child did not crash while publishing the "
                "first ownership record (saved=%1, error=%2)")
                .arg(saved)
                .arg(error)));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray priorBackup("prior backup contents");
    writeFixture(fixture.backupPath, priorBackup);

    const auto crashed = runCrashChild(
        QStringLiteral(
            "crashDuringOwnershipRecordPreservesArtifact"),
        QString(),
        fixture.temporary.path(),
        QStringLiteral("save-crash"),
        QStringLiteral("split-journal-temp-synchronized"),
        2,
        false);
    QCOMPARE(crashed.first, SplitCrashExitCode);

    const QString journalPath = onlyJournalPath(fixture);
    QVERIFY(!journalPath.isEmpty());
    const QString transactionId = QFileInfo(journalPath).fileName();
    const QString artifactPath = QFileInfo(fixture.backupPath)
        .dir().filePath(
            QStringLiteral(".gc-split-%1-source.stage")
                .arg(transactionId));
    const QString interruptedRecord = QDir(journalPath).filePath(
        QStringLiteral("cleanup-0000.json.tmp"));
    QCOMPARE(readBytes(artifactPath), sourceContents());
    QVERIFY(filePresent(interruptedRecord));

    QString recoveryError;
    QVERIFY(!SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError));
    QVERIFY2(
        recoveryError.contains(
            QStringLiteral("unproven"), Qt::CaseInsensitive),
        qPrintable(recoveryError));
    QVERIFY(recoveryError.contains(artifactPath));
    QCOMPARE(readBytes(artifactPath), sourceContents());
    QVERIFY(!filePresent(interruptedRecord));
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), priorBackup);

    QVERIFY(QFile::remove(artifactPath));
    recoveryError.clear();
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), recoveryError),
        qPrintable(recoveryError));
    QString secondRecoveryError;
    QVERIFY2(SplitActivityTransaction::reconcileAll(
        fixture.temporary.path(), secondRecoveryError),
        qPrintable(secondRecoveryError));
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), priorBackup);
    QVERIFY(!filePresent(
        fixture.activities.filePath(firstOutputName())));
    QVERIFY(!filePresent(
        fixture.activities.filePath(secondOutputName())));
    QVERIFY(splitNamespaceIsEmpty(fixture));
    QVERIFY(splitProductionArtifactsAreAbsent(fixture));
}

void TestSplitActivitySave::
crashRecoveryAtDurableTransition_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("occurrence");
    QTest::addColumn<bool>("committed");
    QTest::addColumn<bool>("keepOriginal");
    QTest::addColumn<QString>("recoveryPhase");
    QTest::addColumn<int>("recoveryOccurrence");

    const auto add = [](
        const char *name,
        int occurrence,
        bool committed,
        bool keepOriginal = false,
        const char *recoveryPhase = "",
        int recoveryOccurrence = 1) {
        QString tag = QString::fromLatin1(name);
        if (occurrence != 1) {
            tag += QStringLiteral("-%1").arg(occurrence);
        }
        if (keepOriginal) {
            tag += QStringLiteral("-keep");
        }
        if (*recoveryPhase) {
            tag += QStringLiteral("-recover-%1")
                .arg(QString::fromLatin1(recoveryPhase));
            if (recoveryOccurrence != 1) {
                tag += QStringLiteral("-%1")
                    .arg(recoveryOccurrence);
            }
        }
        QTest::newRow(qPrintable(tag))
            << QString::fromLatin1(name)
            << occurrence
            << committed
            << keepOriginal
            << QString::fromLatin1(recoveryPhase)
            << recoveryOccurrence;
    };

    add("split-transaction-directory-created", 1, false);
    add("split-intent-published", 1, false);
    add("split-source-snapshot-published", 1, false);
    add("split-prior-backup-snapshot-published", 1, false);
    add("split-source-archive-stage-published", 1, false);
    add("split-output-input-synchronized", 1, false);
    add("split-output-input-synchronized", 2, false);
    add("split-output-staged", 2, false);
    add("split-output-staged", 1, false);
    add("split-prepared-manifest-published", 1, false);
    add("split-committing-marker-published", 1, true);
    add("split-before-output-published", 1, true);
    add("split-before-output-published", 2, true);
    add("split-output-published", 1, true);
    add("split-output-published", 2, true);
    add("split-prior-backup-retired", 1, true);
    add("split-source-archived", 1, true);
    add("split-before-output-parents-synchronized", 1, true);
    add("split-output-parents-synchronized", 1, true);
    add("split-source-retired-for-commit", 1, true);
    add("split-committed-marker-published", 1, true);
    add("split-committed-source-retired", 1, true);
    add("split-recovery-manifest-removed", 1, true);
    add("split-transaction-cleaned", 1, true);

    add("split-committing-marker-published", 1, true, true);
    add("split-output-parents-synchronized", 1, true, true);
    add(
        "split-committed-marker-published",
        1,
        true,
        true);

    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "split-recovery-output-stage-retired",
        1);
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "split-recovery-output-stage-retired",
        2);
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "split-recovery-archive-stage-retired");
#ifdef Q_OS_UNIX
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "remove-quarantine-verified",
        1);
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "remove-quarantine-verified",
        3);
#endif
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "split-recovery-manifest-removed");
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "split-recovery-journal-file-removed");
    add(
        "split-prepared-manifest-published",
        1,
        false,
        false,
        "split-recovery-directory-removed");
#ifdef Q_OS_UNIX
    add(
        "split-committed-marker-published",
        1,
        true,
        false,
        "remove-quarantine-verified");
    add(
        "split-source-archive-stage-published",
        1,
        false,
        false,
        "remove-quarantine-verified");
#endif

    // Journal publication itself is a two-step durable protocol. These rows
    // crash after the temporary file is synchronized and after its anchored
    // rename, before the caller can report the phase transition.
    add("split-journal-temp-synchronized", 1, false);
    add("split-journal-temp-synchronized", 5, false);
    add("split-journal-temp-synchronized", 6, false);
    add("split-journal-temp-synchronized", 7, true);
    add("split-journal-file-published", 1, false);
    add("split-journal-file-published", 5, false);
    add("split-journal-file-published", 6, true);
    add("split-journal-file-published", 7, true);
    add(
        "split-journal-temp-synchronized",
        5,
        false,
        false,
        "split-recovery-journal-temp-retired");
}

void TestSplitActivitySave::
crashRecoveryAtDurableTransition()
{
    QFETCH(QString, phase);
    QFETCH(int, occurrence);
    QFETCH(bool, committed);
    QFETCH(bool, keepOriginal);
    QFETCH(QString, recoveryPhase);
    QFETCH(int, recoveryOccurrence);

    const QString childRoot =
        qEnvironmentVariable(CrashRootEnvironment);
    const QString childMode =
        qEnvironmentVariable(CrashModeEnvironment);
    if (!childRoot.isEmpty() && !childMode.isEmpty()) {
        if (childMode.startsWith(
                QStringLiteral("recover"))) {
            QString recoveryError;
            QVERIFY2(
                SplitActivityTransaction::reconcileAll(
                    childRoot, recoveryError),
                qPrintable(recoveryError));
            if (childMode
                == QStringLiteral("recover-crash")) {
                QFAIL(
                    "Recovery child did not reach its crash transition");
            }
            return;
        }

        const QDir root(childRoot);
        const QDir activities(
            root.filePath(QStringLiteral("activities")));
        const bool childKeepOriginal =
            qEnvironmentVariableIntValue(
                CrashKeepOriginalEnvironment) != 0;
        QStringList published;
        QString error;
        const bool saved = saveSplitActivityFiles(
            activities,
            activities.filePath(sourceFileName()),
            root.filePath(
                QStringLiteral(
                    "backup/original-activity.json.bak")),
            {
                outputWriting(
                    firstOutputName(),
                    QByteArray("first output")),
                outputWriting(
                    secondOutputName(),
                    QByteArray("second output"))
            },
            childKeepOriginal,
            published,
            error);
        QFAIL(qPrintable(
            QStringLiteral(
                "Save child did not reach %1/%2 "
                "(saved=%3, error=%4)")
                .arg(phase)
                .arg(occurrence)
                .arg(saved)
                .arg(error)));
        return;
    }

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray priorBackup(
        "prior backup contents");
    writeFixture(fixture.backupPath, priorBackup);

    const QString dataTag =
        QString::fromLatin1(QTest::currentDataTag());
    const auto crashed = runCrashChild(
        QStringLiteral(
            "crashRecoveryAtDurableTransition"),
        dataTag,
        fixture.temporary.path(),
        QStringLiteral("save-crash"),
        phase,
        occurrence,
        keepOriginal);
    QCOMPARE(crashed.first, SplitCrashExitCode);

    if (phase
            == QStringLiteral(
                "split-output-parents-synchronized")
        && recoveryPhase.isEmpty()) {
        const QDir namespaceDirectory(
            fixture.transactionNamespace());
        const QFileInfoList journals =
            namespaceDirectory.entryInfoList(
                QDir::Dirs | QDir::Hidden | QDir::System
                    | QDir::NoDotAndDotDot);
        QCOMPARE(journals.size(), 1);
        QVERIFY(!filePresent(
            QDir(journals.constFirst().absoluteFilePath())
                .filePath(QStringLiteral("COMMITTED"))));
    }

    if (!recoveryPhase.isEmpty()) {
        const auto recoveryCrash = runCrashChild(
            QStringLiteral(
                "crashRecoveryAtDurableTransition"),
            dataTag,
            fixture.temporary.path(),
            QStringLiteral("recover-crash"),
            recoveryPhase,
            recoveryOccurrence,
            keepOriginal);
        QCOMPARE(
            recoveryCrash.first,
            SplitCrashExitCode);
    }

    const auto recovered = runCrashChild(
        QStringLiteral(
            "crashRecoveryAtDurableTransition"),
        dataTag,
        fixture.temporary.path(),
        QStringLiteral("recover"),
        QString(),
        1,
        keepOriginal);
    QCOMPARE(recovered.first, 0);
    const auto recoveredAgain = runCrashChild(
        QStringLiteral(
            "crashRecoveryAtDurableTransition"),
        dataTag,
        fixture.temporary.path(),
        QStringLiteral("recover"),
        QString(),
        1,
        keepOriginal);
    QCOMPARE(recoveredAgain.first, 0);

    const QString firstTarget =
        fixture.activities.filePath(firstOutputName());
    const QString secondTarget =
        fixture.activities.filePath(secondOutputName());
    if (committed) {
        QCOMPARE(
            readBytes(firstTarget),
            QByteArray("first output"));
        QCOMPARE(
            readBytes(secondTarget),
            QByteArray("second output"));
        if (keepOriginal) {
            QCOMPARE(
                readBytes(fixture.sourcePath),
                sourceContents());
            QCOMPARE(
                readBytes(fixture.backupPath),
                priorBackup);
        } else {
            QVERIFY(!filePresent(fixture.sourcePath));
            QCOMPARE(
                readBytes(fixture.backupPath),
                sourceContents());
        }
    } else {
        QCOMPARE(
            readBytes(fixture.sourcePath),
            sourceContents());
        QCOMPARE(
            readBytes(fixture.backupPath),
            priorBackup);
        QVERIFY(!filePresent(firstTarget));
        QVERIFY(!filePresent(secondTarget));
    }
    QVERIFY(splitNamespaceIsEmpty(fixture));
    QVERIFY(splitProductionArtifactsAreAbsent(fixture));
}

QTEST_MAIN(TestSplitActivitySave)
#include "testSplitActivitySave.moc"
