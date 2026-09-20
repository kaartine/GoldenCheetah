/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDevelopmentAssets.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QtConcurrent>
#include <QtEndian>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr quint32 GlbMagic = 0x46546c67;
constexpr quint32 GlbVersion = 2;
constexpr quint32 JsonChunk = 0x4e4f534a;
constexpr quint32 BinaryChunk = 0x004e4942;
constexpr qsizetype MaximumManifestBytes = 1024 * 1024;
constexpr qsizetype MaximumGlbBytes = 64 * 1024 * 1024;
constexpr int MaximumRuntimeVariants = 32;
constexpr int MaximumVariantRootNodes = 8;
const QString ManifestRelativeDirectory = QStringLiteral(
        "contrib/workout-game-assets/manifests");
const QString GeneratedRelativeDirectory = QStringLiteral(
        "contrib/workout-game-assets/generated");

class BuildError : public std::runtime_error
{
public:
    explicit BuildError(const QString &message) :
        std::runtime_error(message.toUtf8().constData())
    {
    }
};

struct ParsedAsset {
    QString assetId;
    QByteArray glb;
    QJsonObject glbDocument;
    qsizetype glbTailOffset = 0;
    qsizetype binaryChunkBytes = 0;
    QHash<QString, QStringList> variants;
    QHash<QString, QVariantMap> materials;
};

quint32 readLittleEndian32(const QByteArray &bytes, qsizetype offset)
{
    if (offset < 0 || offset + 4 > bytes.size()) {
        throw BuildError(QStringLiteral("truncated GLB integer"));
    }
    return qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

void appendLittleEndian32(QByteArray &bytes, quint32 value)
{
    char encoded[4];
    qToLittleEndian<quint32>(
            value, reinterpret_cast<uchar *>(encoded));
    bytes.append(encoded, 4);
}

QString canonicalDirectory(const QString &path, const QString &description)
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QFileInfo info(absolute);
    const QString canonical = info.canonicalFilePath();
    if (!info.isDir() || info.isSymLink() || canonical.isEmpty()
            || canonical != absolute) {
        throw BuildError(QStringLiteral("invalid %1: %2")
                .arg(description, path));
    }
    return canonical;
}

QString validateRepositoryRelativePath(
        const QString &relative,
        const QString &requiredDirectory)
{
    if (relative.isEmpty() || relative.startsWith(QLatin1Char('/'))
            || relative.contains(QLatin1Char('\\'))
            || QDir::cleanPath(relative) != relative
            || !(relative == requiredDirectory
                 || relative.startsWith(requiredDirectory
                         + QLatin1Char('/')))) {
        throw BuildError(QStringLiteral("unsafe asset path: %1").arg(relative));
    }
    const QStringList parts = relative.split(QLatin1Char('/'));
    if (parts.contains(QStringLiteral("."))
            || parts.contains(QStringLiteral(".."))
            || parts.contains(QString())) {
        throw BuildError(QStringLiteral("unsafe asset path: %1").arg(relative));
    }
    return relative;
}

QByteArray readRepositoryFile(
        const QString &workspace,
        qulonglong workspaceDevice,
        qulonglong workspaceInode,
        const QString &relative,
        const QString &requiredDirectory,
        qsizetype maximumBytes,
        const QString &description)
{
    validateRepositoryRelativePath(relative, requiredDirectory);
#if defined(Q_OS_UNIX)
    int descriptor = ::open(
            QFile::encodeName(workspace).constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw BuildError(QStringLiteral("cannot anchor %1 workspace")
                .arg(description));
    }
    struct stat workspaceStat{};
    if (::fstat(descriptor, &workspaceStat) != 0
            || static_cast<qulonglong>(workspaceStat.st_dev) != workspaceDevice
            || static_cast<qulonglong>(workspaceStat.st_ino) != workspaceInode) {
        ::close(descriptor);
        throw BuildError(QStringLiteral("development asset workspace changed"));
    }
    const QStringList parts = relative.split(QLatin1Char('/'));
    for (int index = 0; index < parts.size(); ++index) {
        const bool final = index + 1 == parts.size();
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW
                | (final ? O_NONBLOCK : O_DIRECTORY);
        const QByteArray encoded = QFile::encodeName(parts.at(index));
        const int next = ::openat(descriptor, encoded.constData(), flags);
        ::close(descriptor);
        descriptor = next;
        if (descriptor < 0) {
            throw BuildError(QStringLiteral("cannot securely open %1: %2")
                    .arg(description, relative));
        }
    }

    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode)
            || before.st_size < 0 || before.st_size > maximumBytes) {
        ::close(descriptor);
        throw BuildError(QStringLiteral("invalid %1: %2")
                .arg(description, relative));
    }
    QFile input;
    if (!input.open(
            descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        throw BuildError(QStringLiteral("cannot read %1: %2")
                .arg(description, relative));
    }
    const QByteArray bytes = input.read(maximumBytes + 1);
    struct stat after{};
    if (::fstat(input.handle(), &after) != 0
            || before.st_dev != after.st_dev
            || before.st_ino != after.st_ino
            || before.st_size != after.st_size
            || before.st_mtime != after.st_mtime
            || bytes.size() != before.st_size
            || bytes.size() > maximumBytes) {
        throw BuildError(QStringLiteral("%1 changed while reading: %2")
                .arg(description, relative));
    }
    return bytes;
#else
    Q_UNUSED(workspaceDevice)
    Q_UNUSED(workspaceInode)
    const QString absolute = QDir::cleanPath(QDir(workspace).filePath(relative));
    const QFileInfo info(absolute);
    const QString canonical = info.canonicalFilePath();
    if (!info.isFile() || info.isSymLink() || canonical.isEmpty()
            || canonical != absolute
            || !canonical.startsWith(workspace + QDir::separator())
            || info.size() < 0
            || info.size() > maximumBytes) {
        throw BuildError(QStringLiteral("invalid %1: %2")
                .arg(description, relative));
    }
    QFile input(absolute);
    if (!input.open(QIODevice::ReadOnly)) {
        throw BuildError(QStringLiteral("cannot read %1: %2")
                .arg(description, relative));
    }
    const QByteArray bytes = input.read(maximumBytes + 1);
    if (bytes.size() != info.size() || bytes.size() > maximumBytes) {
        throw BuildError(QStringLiteral("%1 changed or exceeded its limit: %2")
                .arg(description, relative));
    }
    const QFileInfo after(absolute);
    if (!after.isFile() || after.isSymLink()
            || after.canonicalFilePath() != canonical
            || after.size() != info.size()
            || after.lastModified() != info.lastModified()) {
        throw BuildError(QStringLiteral("%1 changed while reading: %2")
                .arg(description, relative));
    }
    return bytes;
#endif
}

QJsonObject parseJsonObject(
        const QByteArray &bytes,
        const QString &description)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw BuildError(QStringLiteral("invalid JSON in %1: %2")
                .arg(description, error.errorString()));
    }
    return document.object();
}

double srgbToLinear(double value)
{
    return value <= 0.04045
            ? value / 12.92
            : std::pow((value + 0.055) / 1.055, 2.4);
}

QJsonArray linearColor(const QString &color, double alpha)
{
    static const QRegularExpression pattern(
            QStringLiteral("^#[0-9a-f]{6}$"));
    if (!pattern.match(color).hasMatch()) {
        throw BuildError(QStringLiteral("invalid sRGB material color: %1")
                .arg(color));
    }
    QJsonArray result;
    for (int offset : {1, 3, 5}) {
        bool ok = false;
        const double channel = color.mid(offset, 2).toInt(&ok, 16) / 255.0;
        if (!ok) {
            throw BuildError(QStringLiteral("invalid sRGB material color: %1")
                    .arg(color));
        }
        result.append(srgbToLinear(channel));
    }
    result.append(alpha);
    return result;
}

qint64 jsonInteger(
        const QJsonValue &value,
        qint64 minimum,
        qint64 maximum,
        const QString &description)
{
    if (!value.isDouble()) {
        throw BuildError(QStringLiteral("missing integer %1").arg(description));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < static_cast<double>(minimum)
            || number > static_cast<double>(maximum)) {
        throw BuildError(QStringLiteral("invalid integer %1").arg(description));
    }
    return static_cast<qint64>(number);
}

QSet<QString> jsonStringSet(
        const QJsonValue &value,
        int maximumItems,
        int maximumLength,
        const QString &description,
        bool allowEmpty = true)
{
    if (!value.isArray()) {
        throw BuildError(QStringLiteral("invalid string array %1")
                .arg(description));
    }
    const QJsonArray array = value.toArray();
    if (array.size() > maximumItems) {
        throw BuildError(QStringLiteral("too many entries in %1")
                .arg(description));
    }
    QSet<QString> result;
    for (const QJsonValue &entry : array) {
        if (!entry.isString()) {
            throw BuildError(QStringLiteral("invalid string in %1")
                    .arg(description));
        }
        const QString text = entry.toString();
        if ((!allowEmpty && text.isEmpty()) || text.size() > maximumLength
                || result.contains(text)) {
            throw BuildError(QStringLiteral("invalid or duplicate string in %1")
                    .arg(description));
        }
        result.insert(text);
    }
    return result;
}

void requireObjectShape(
        const QJsonObject &object,
        const QSet<QString> &required,
        const QSet<QString> &allowed,
        const QString &description)
{
    for (const QString &key : required) {
        if (!object.contains(key)) {
            throw BuildError(QStringLiteral("missing %1 field: %2")
                    .arg(description, key));
        }
    }
    for (auto field = object.constBegin(); field != object.constEnd(); ++field) {
        if (!allowed.contains(field.key())) {
            throw BuildError(QStringLiteral("unknown %1 field: %2")
                    .arg(description, field.key()));
        }
    }
}

QString requiredString(
        const QJsonObject &object,
        const QString &key,
        const QString &description,
        int maximumLength = 4096)
{
    const QJsonValue value = object.value(key);
    const QString text = value.toString();
    if (!value.isString() || text.isEmpty() || text.size() > maximumLength) {
        throw BuildError(QStringLiteral("invalid %1 string: %2")
                .arg(description, key));
    }
    return text;
}

void validateManifestContract(const QJsonObject &manifest)
{
    const QSet<QString> topLevel = {
        QStringLiteral("manifestVersion"), QStringLiteral("assetId"),
        QStringLiteral("displayName"),
        QStringLiteral("role"), QStringLiteral("source"),
        QStringLiteral("license"), QStringLiteral("files"),
        QStringLiteral("processing"), QStringLiteral("technical"),
        QStringLiteral("materialOverrides"), QStringLiteral("physics"),
        QStringLiteral("runtimeVariants"), QStringLiteral("review"),
    };
    requireObjectShape(manifest,
            {QStringLiteral("manifestVersion"), QStringLiteral("assetId"),
             QStringLiteral("displayName"),
             QStringLiteral("role"), QStringLiteral("source"),
             QStringLiteral("license"), QStringLiteral("files"),
             QStringLiteral("technical"), QStringLiteral("physics"),
             QStringLiteral("review")},
            topLevel, QStringLiteral("manifest"));
    const QJsonValue manifestVersion = manifest.value(
            QStringLiteral("manifestVersion"));
    if (!manifestVersion.isDouble() || manifestVersion.toDouble() != 1.0) {
        throw BuildError(QStringLiteral("unsupported manifest version"));
    }
    requiredString(manifest, QStringLiteral("assetId"), QStringLiteral("manifest"), 128);
    requiredString(manifest, QStringLiteral("displayName"), QStringLiteral("manifest"));
    const QSet<QString> roles = {
        QStringLiteral("rider-bike"), QStringLiteral("trail-tile"),
        QStringLiteral("feature"), QStringLiteral("terrain"),
        QStringLiteral("vegetation"), QStringLiteral("prop"),
        QStringLiteral("effect"), QStringLiteral("texture"),
        QStringLiteral("reference"),
    };
    if (!manifest.value(QStringLiteral("role")).isString()
            || !roles.contains(manifest.value(QStringLiteral("role")).toString())) {
        throw BuildError(QStringLiteral("invalid manifest role"));
    }

    if (!manifest.value(QStringLiteral("source")).isObject()) {
        throw BuildError(QStringLiteral("invalid manifest source"));
    }
    const QJsonObject source = manifest.value(QStringLiteral("source")).toObject();
    const QSet<QString> sourceKeys = {
        QStringLiteral("kind"), QStringLiteral("provider"),
        QStringLiteral("author"), QStringLiteral("url"),
        QStringLiteral("downloadUrl"), QStringLiteral("providerAssetId"),
        QStringLiteral("providerRevision"), QStringLiteral("retrievedAt"),
        QStringLiteral("originalFileName"), QStringLiteral("originalSha256"),
        QStringLiteral("generator"), QStringLiteral("generatorVersion"),
        QStringLiteral("promptOrScript"),
    };
    requireObjectShape(source,
            {QStringLiteral("kind"), QStringLiteral("provider"),
             QStringLiteral("author"), QStringLiteral("url"),
             QStringLiteral("retrievedAt")},
            sourceKeys, QStringLiteral("source"));
    const QString sourceKind = requiredString(
            source, QStringLiteral("kind"), QStringLiteral("source"), 32);
    if (!QSet<QString>({QStringLiteral("project-authored"),
                       QStringLiteral("external"),
                       QStringLiteral("generated")}).contains(sourceKind)) {
        throw BuildError(QStringLiteral("invalid source kind"));
    }
    for (const QString &key : {QStringLiteral("provider"),
            QStringLiteral("author"), QStringLiteral("url"),
            QStringLiteral("retrievedAt")}) {
        requiredString(source, key, QStringLiteral("source"));
    }
    static const QRegularExpression digestPattern(
            QStringLiteral("^[a-f0-9]{64}$"));
    const bool hasOriginalDigest = source.contains(
            QStringLiteral("originalSha256"));
    if ((sourceKind == QStringLiteral("external")) != hasOriginalDigest
            || (hasOriginalDigest
                && (!source.value(QStringLiteral("originalSha256")).isString()
                    || !digestPattern.match(source.value(
                        QStringLiteral("originalSha256")).toString()).hasMatch()))) {
        throw BuildError(QStringLiteral("invalid source original digest"));
    }
    for (const QString &key : sourceKeys) {
        if (source.contains(key) && key != QStringLiteral("originalSha256")
                && !source.value(key).isString()) {
            throw BuildError(QStringLiteral("invalid source field type: %1")
                    .arg(key));
        }
    }

    if (!manifest.value(QStringLiteral("license")).isObject()
            || !manifest.value(QStringLiteral("review")).isObject()) {
        throw BuildError(QStringLiteral("invalid license or review object"));
    }
    const QJsonObject license = manifest.value(QStringLiteral("license")).toObject();
    const QSet<QString> licenseKeys = {
        QStringLiteral("spdxId"), QStringLiteral("licenseUrl"),
        QStringLiteral("attribution"), QStringLiteral("modificationAllowed"),
        QStringLiteral("redistributionAllowed"), QStringLiteral("decision"),
        QStringLiteral("policyVersion"), QStringLiteral("archivedLicensePath"),
        QStringLiteral("distributionScopes"), QStringLiteral("conditions"),
        QStringLiteral("reviewNotes"),
    };
    requireObjectShape(license,
            {QStringLiteral("spdxId"), QStringLiteral("licenseUrl"),
             QStringLiteral("modificationAllowed"),
             QStringLiteral("redistributionAllowed"),
             QStringLiteral("decision")},
            licenseKeys, QStringLiteral("license"));
    requiredString(license, QStringLiteral("spdxId"), QStringLiteral("license"), 128);
    requiredString(license, QStringLiteral("licenseUrl"), QStringLiteral("license"));
    if (!license.value(QStringLiteral("modificationAllowed")).isBool()
            || !license.value(QStringLiteral("redistributionAllowed")).isBool()) {
        throw BuildError(QStringLiteral("invalid license permissions"));
    }
    for (const QString &key : {QStringLiteral("attribution"),
            QStringLiteral("policyVersion"),
            QStringLiteral("archivedLicensePath"),
            QStringLiteral("reviewNotes")}) {
        if (license.contains(key) && !license.value(key).isString()) {
            throw BuildError(QStringLiteral("invalid license field type: %1")
                    .arg(key));
        }
    }
    if (license.contains(QStringLiteral("distributionScopes"))) {
        const QSet<QString> scopes = jsonStringSet(
                license.value(QStringLiteral("distributionScopes")),
                3, 32, QStringLiteral("license.distributionScopes"));
        const QSet<QString> allowedScopes = {
            QStringLiteral("source"), QStringLiteral("appimage"),
            QStringLiteral("screenshots-video"),
        };
        for (const QString &scope : scopes) {
            if (!allowedScopes.contains(scope)) {
                throw BuildError(QStringLiteral("invalid license distribution scope"));
            }
        }
    }
    const QString decision = requiredString(
            license, QStringLiteral("decision"), QStringLiteral("license"), 32);
    if (!QSet<QString>({QStringLiteral("allow"),
                       QStringLiteral("conditional"),
                       QStringLiteral("reject")}).contains(decision)) {
        throw BuildError(QStringLiteral("invalid license decision"));
    }
    const QJsonObject review = manifest.value(QStringLiteral("review")).toObject();
    const QSet<QString> reviewKeys = {
        QStringLiteral("status"), QStringLiteral("trademarkStatus"),
        QStringLiteral("personReleaseStatus"),
        QStringLiteral("propertyReleaseStatus"), QStringLiteral("reviewer"),
        QStringLiteral("reviewedAt"), QStringLiteral("notes"),
    };
    requireObjectShape(review, {QStringLiteral("status")}, reviewKeys,
            QStringLiteral("review"));
    const QString reviewStatus = requiredString(
            review, QStringLiteral("status"), QStringLiteral("review"), 32);
    if (!QSet<QString>({QStringLiteral("candidate"),
                       QStringLiteral("approved"),
                       QStringLiteral("rejected")}).contains(reviewStatus)) {
        throw BuildError(QStringLiteral("invalid review status"));
    }
    const QSet<QString> clearanceStatuses = {
        QStringLiteral("not-applicable"), QStringLiteral("clear"),
        QStringLiteral("review-required"), QStringLiteral("rejected"),
    };
    for (const QString &key : {QStringLiteral("trademarkStatus"),
            QStringLiteral("personReleaseStatus"),
            QStringLiteral("propertyReleaseStatus")}) {
        if (review.contains(key)
                && (!review.value(key).isString()
                    || !clearanceStatuses.contains(
                        review.value(key).toString()))) {
            throw BuildError(QStringLiteral("invalid review clearance: %1")
                    .arg(key));
        }
    }
    for (const QString &key : {QStringLiteral("reviewer"),
            QStringLiteral("reviewedAt"), QStringLiteral("notes")}) {
        if (review.contains(key) && !review.value(key).isString()) {
            throw BuildError(QStringLiteral("invalid review field type: %1")
                    .arg(key));
        }
    }
    const QJsonValue conditions = license.value(QStringLiteral("conditions"));
    if (conditions.isUndefined()) {
        // Optional for an unconditional license.
    } else {
        jsonStringSet(conditions, 64, 4096, QStringLiteral("license.conditions"));
    }
    if (decision == QStringLiteral("reject")
            || reviewStatus == QStringLiteral("rejected")
            || !license.value(QStringLiteral("modificationAllowed")).toBool()
            || !license.value(QStringLiteral("redistributionAllowed")).toBool()
            || (decision == QStringLiteral("conditional")
                && (reviewStatus != QStringLiteral("approved")
                    || !conditions.isArray()
                    || conditions.toArray().isEmpty()))) {
        throw BuildError(QStringLiteral("asset license is not approved for development"));
    }

    if (manifest.contains(QStringLiteral("processing"))) {
        if (!manifest.value(QStringLiteral("processing")).isObject()) {
            throw BuildError(QStringLiteral("invalid processing object"));
        }
        const QJsonObject processing = manifest.value(
                QStringLiteral("processing")).toObject();
        requireObjectShape(processing, {},
                {QStringLiteral("tool"), QStringLiteral("toolVersion"),
                 QStringLiteral("steps")}, QStringLiteral("processing"));
        for (const QString &key : {QStringLiteral("tool"),
                QStringLiteral("toolVersion")}) {
            if (processing.contains(key) && !processing.value(key).isString()) {
                throw BuildError(QStringLiteral("invalid processing field type: %1")
                        .arg(key));
            }
        }
        if (processing.contains(QStringLiteral("steps"))) {
            jsonStringSet(processing.value(QStringLiteral("steps")),
                    256, 4096, QStringLiteral("processing.steps"), true);
        }
    }

    if (!manifest.value(QStringLiteral("technical")).isObject()
            || !manifest.value(QStringLiteral("files")).isArray()) {
        throw BuildError(QStringLiteral("invalid technical or file inventory"));
    }
    const QJsonObject technical = manifest.value(QStringLiteral("technical")).toObject();
    const QSet<QString> technicalKeys = {
        QStringLiteral("format"), QStringLiteral("unitMeters"),
        QStringLiteral("upAxis"), QStringLiteral("forwardAxis"),
        QStringLiteral("trianglesLod0"), QStringLiteral("trianglesLod1"),
        QStringLiteral("trianglesLod2"), QStringLiteral("glbBytes"),
        QStringLiteral("materials"), QStringLiteral("textureBytes"),
        QStringLiteral("nodes"), QStringLiteral("animations"),
        QStringLiteral("allowedExtensions"), QStringLiteral("boundsMeters"),
        QStringLiteral("sockets"), QStringLiteral("budgets"),
    };
    requireObjectShape(technical,
            {QStringLiteral("format"), QStringLiteral("unitMeters"),
             QStringLiteral("upAxis"), QStringLiteral("forwardAxis")},
            technicalKeys, QStringLiteral("technical"));
    const QString format = technical.value(QStringLiteral("format")).toString();
    if (!QSet<QString>({QStringLiteral("glb"), QStringLiteral("png"),
                       QStringLiteral("webp"), QStringLiteral("blend"),
                       QStringLiteral("other")}).contains(format)
            || !technical.value(QStringLiteral("unitMeters")).isDouble()
            || technical.value(QStringLiteral("unitMeters")).toDouble() != 1.0
            || technical.value(QStringLiteral("upAxis")).toString()
                != QStringLiteral("+Y")
            || technical.value(QStringLiteral("forwardAxis")).toString()
                != QStringLiteral("+Z")) {
        throw BuildError(QStringLiteral("invalid technical coordinate contract"));
    }
    if (format == QStringLiteral("glb")) {
        for (const QString &key : {QStringLiteral("trianglesLod0"),
                QStringLiteral("glbBytes"), QStringLiteral("materials"),
                QStringLiteral("textureBytes"), QStringLiteral("nodes"),
                QStringLiteral("animations"), QStringLiteral("allowedExtensions"),
                QStringLiteral("boundsMeters"), QStringLiteral("budgets")}) {
            if (!technical.contains(key)) {
                throw BuildError(QStringLiteral("missing GLB technical field: %1")
                        .arg(key));
            }
        }
    }
}

QList<double> finiteVector(
        const QJsonValue &value,
        int size,
        const QString &description)
{
    if (!value.isArray() || value.toArray().size() != size) {
        throw BuildError(QStringLiteral("invalid vector %1").arg(description));
    }
    QList<double> result;
    result.reserve(size);
    for (const QJsonValue &component : value.toArray()) {
        const double number = component.toDouble(
                std::numeric_limits<double>::quiet_NaN());
        if (!component.isDouble() || !std::isfinite(number)) {
            throw BuildError(QStringLiteral("non-finite vector %1")
                    .arg(description));
        }
        result.append(number);
    }
    return result;
}

bool vectorsClose(
        const QList<double> &left,
        const QList<double> &right,
        double tolerance = 1e-5)
{
    if (left.size() != right.size()) return false;
    for (int index = 0; index < left.size(); ++index) {
        if (std::abs(left.at(index) - right.at(index)) > tolerance) return false;
    }
    return true;
}

void requireArrayField(
        const QJsonObject &object,
        const QString &key,
        bool required = false)
{
    if ((required || object.contains(key)) && !object.value(key).isArray()) {
        throw BuildError(QStringLiteral("invalid array field: %1").arg(key));
    }
}

void validateGlbDocument(
        const QJsonObject &document,
        qsizetype glbBytes,
        qsizetype binaryChunkBytes,
        const QJsonObject &manifest,
        const QString &assetId)
{
    const QJsonObject technical = manifest.value(
            QStringLiteral("technical")).toObject();
    const QJsonObject budgets = technical.value(
            QStringLiteral("budgets")).toObject();
    if (technical.value(QStringLiteral("format")).toString()
                != QStringLiteral("glb")
            || jsonInteger(technical.value(QStringLiteral("glbBytes")),
                    1, MaximumGlbBytes, QStringLiteral("technical.glbBytes"))
                != glbBytes
            || glbBytes > jsonInteger(
                    budgets.value(QStringLiteral("maxGlbBytes")),
                    1, MaximumGlbBytes,
                    QStringLiteral("budgets.maxGlbBytes"))) {
        throw BuildError(QStringLiteral("GLB byte contract mismatch: %1")
                .arg(assetId));
    }
    if (!document.value(QStringLiteral("asset")).isObject()
            || document.value(QStringLiteral("asset")).toObject().value(
            QStringLiteral("version")).toString() != QStringLiteral("2.0")) {
        throw BuildError(QStringLiteral("asset is not glTF 2.0: %1")
                .arg(assetId));
    }
    const QSet<QString> supportedTopLevelFields = {
        QStringLiteral("asset"), QStringLiteral("buffers"),
        QStringLiteral("bufferViews"), QStringLiteral("accessors"),
        QStringLiteral("meshes"), QStringLiteral("nodes"),
        QStringLiteral("scenes"), QStringLiteral("scene"),
        QStringLiteral("materials"), QStringLiteral("images"),
        QStringLiteral("textures"), QStringLiteral("samplers"),
        QStringLiteral("skins"), QStringLiteral("animations"),
        QStringLiteral("cameras"), QStringLiteral("extensions"),
        QStringLiteral("extensionsUsed"),
        QStringLiteral("extensionsRequired"),
    };
    for (auto field = document.constBegin(); field != document.constEnd(); ++field) {
        if (!supportedTopLevelFields.contains(field.key())) {
            throw BuildError(QStringLiteral("unsupported GLB field: %1")
                    .arg(field.key()));
        }
    }

    const QSet<QString> allowedExtensions = jsonStringSet(
            technical.value(QStringLiteral("allowedExtensions")),
            64, 128, QStringLiteral("technical.allowedExtensions"));
    const QSet<QString> usedExtensions = jsonStringSet(
            document.contains(QStringLiteral("extensionsUsed"))
                ? document.value(QStringLiteral("extensionsUsed"))
                : QJsonValue(QJsonArray()),
            64, 128, QStringLiteral("extensionsUsed"));
    const QSet<QString> requiredExtensions = jsonStringSet(
            document.contains(QStringLiteral("extensionsRequired"))
                ? document.value(QStringLiteral("extensionsRequired"))
                : QJsonValue(QJsonArray()),
            64, 128, QStringLiteral("extensionsRequired"));
    for (const QString &extension : requiredExtensions) {
        if (!usedExtensions.contains(extension)) {
            throw BuildError(QStringLiteral("undeclared required GLB extension: %1")
                    .arg(extension));
        }
    }
    for (const QString &extension : usedExtensions) {
        if (!allowedExtensions.contains(extension)) {
            throw BuildError(QStringLiteral("non-allowlisted GLB extension: %1")
                    .arg(extension));
        }
    }
    if (!usedExtensions.isEmpty() || !requiredExtensions.isEmpty()) {
        throw BuildError(QStringLiteral("GLB extensions are not supported: %1")
                .arg(assetId));
    }
    for (const QString &key : {QStringLiteral("buffers"),
            QStringLiteral("bufferViews"), QStringLiteral("images"),
            QStringLiteral("meshes"), QStringLiteral("nodes"),
            QStringLiteral("scenes"), QStringLiteral("accessors"),
            QStringLiteral("materials"), QStringLiteral("animations"),
            QStringLiteral("textures"), QStringLiteral("samplers"),
            QStringLiteral("skins")}) {
        requireArrayField(document, key,
                key == QStringLiteral("buffers")
                || key == QStringLiteral("nodes")
                || key == QStringLiteral("scenes"));
    }
    if (document.contains(QStringLiteral("cameras"))
            && (!document.value(QStringLiteral("cameras")).isArray()
                || !document.value(QStringLiteral("cameras")).toArray().isEmpty())) {
        throw BuildError(QStringLiteral("GLB contains an invalid camera collection: %1")
                .arg(assetId));
    }
    if (document.contains(QStringLiteral("extensions"))
            && !document.value(QStringLiteral("extensions")).isObject()) {
        throw BuildError(QStringLiteral("GLB extensions field is not an object: %1")
                .arg(assetId));
    }
    if (!document.value(QStringLiteral("extensions")).toObject().isEmpty()) {
        throw BuildError(QStringLiteral("GLB extension payloads are not supported: %1")
                .arg(assetId));
    }
    if (document.value(QStringLiteral("extensions")).toObject().contains(
                QStringLiteral("KHR_lights_punctual"))) {
        throw BuildError(QStringLiteral("GLB contains a camera or light: %1")
                .arg(assetId));
    }
    for (const QString &key : {QStringLiteral("images"),
            QStringLiteral("textures"), QStringLiteral("samplers"),
            QStringLiteral("skins"), QStringLiteral("animations")}) {
        if (!document.value(key).toArray().isEmpty()) {
            throw BuildError(QStringLiteral("unsupported non-empty GLB collection: %1")
                    .arg(key));
        }
    }

    const QJsonArray buffers = document.value(QStringLiteral("buffers")).toArray();
    if (buffers.size() != 1 || !buffers.first().isObject()
            || binaryChunkBytes <= 0) {
        throw BuildError(QStringLiteral("GLB must contain one embedded buffer: %1")
                .arg(assetId));
    }
    const QJsonObject buffer = buffers.first().toObject();
    if (buffer.contains(QStringLiteral("uri"))) {
        throw BuildError(QStringLiteral("GLB contains an external buffer URI: %1")
                .arg(assetId));
    }
    const qint64 declaredBufferBytes = jsonInteger(
            buffer.value(QStringLiteral("byteLength")),
            0, MaximumGlbBytes, QStringLiteral("buffer.byteLength"));
    if (declaredBufferBytes > binaryChunkBytes
            || binaryChunkBytes - declaredBufferBytes > 3) {
        throw BuildError(QStringLiteral("GLB binary buffer length mismatch: %1")
                .arg(assetId));
    }

    const QJsonArray bufferViews = document.value(
            QStringLiteral("bufferViews")).toArray();
    for (const QJsonValue &value : bufferViews) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid GLB buffer view: %1")
                    .arg(assetId));
        }
        const QJsonObject view = value.toObject();
        if (jsonInteger(view.value(QStringLiteral("buffer")), 0, 0,
                        QStringLiteral("bufferView.buffer")) != 0) {
            throw BuildError(QStringLiteral("invalid GLB buffer reference: %1")
                    .arg(assetId));
        }
        const qint64 offset = view.contains(QStringLiteral("byteOffset"))
                ? jsonInteger(view.value(QStringLiteral("byteOffset")),
                    0, declaredBufferBytes,
                    QStringLiteral("bufferView.byteOffset")) : 0;
        const qint64 length = jsonInteger(
                view.value(QStringLiteral("byteLength")),
                0, declaredBufferBytes,
                QStringLiteral("bufferView.byteLength"));
        if (offset > declaredBufferBytes - length) {
            throw BuildError(QStringLiteral("GLB buffer view exceeds buffer: %1")
                    .arg(assetId));
        }
    }

    qint64 textureBytes = 0;
    const QJsonArray images = document.value(QStringLiteral("images")).toArray();
    for (const QJsonValue &value : images) {
        const QJsonObject image = value.toObject();
        if (!value.isObject() || image.contains(QStringLiteral("uri"))) {
            throw BuildError(QStringLiteral("GLB image URI is not allowed: %1")
                    .arg(assetId));
        }
        const qint64 viewIndex = jsonInteger(
                image.value(QStringLiteral("bufferView")),
                0, std::max<qsizetype>(0, bufferViews.size() - 1),
                QStringLiteral("image.bufferView"));
        if (bufferViews.isEmpty()) {
            throw BuildError(QStringLiteral("GLB image lacks a buffer view: %1")
                    .arg(assetId));
        }
        textureBytes += jsonInteger(
                bufferViews.at(viewIndex).toObject().value(
                    QStringLiteral("byteLength")),
                0, declaredBufferBytes,
                QStringLiteral("image byteLength"));
    }
    if (textureBytes != jsonInteger(
                technical.value(QStringLiteral("textureBytes")),
                0, MaximumGlbBytes, QStringLiteral("technical.textureBytes"))
            || textureBytes > jsonInteger(
                budgets.value(QStringLiteral("maxTextureBytes")),
                0, MaximumGlbBytes,
                QStringLiteral("budgets.maxTextureBytes"))) {
        throw BuildError(QStringLiteral("GLB texture budget mismatch: %1")
                .arg(assetId));
    }

    const QJsonArray meshes = document.value(QStringLiteral("meshes")).toArray();
    const QJsonArray nodes = document.value(QStringLiteral("nodes")).toArray();
    const QSet<QString> expectedNodes = jsonStringSet(
            technical.value(QStringLiteral("nodes")),
            4096, 128, QStringLiteral("technical.nodes"), false);
    QSet<QString> nodeNames;
    QHash<int, int> parents;
    QList<QList<double>> nodeTranslations;
    int rootMetadataNodes = 0;
    for (int nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const QJsonValue value = nodes.at(nodeIndex);
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid GLB node: %1").arg(assetId));
        }
        const QJsonObject node = value.toObject();
        requireArrayField(node, QStringLiteral("children"));
        if (node.contains(QStringLiteral("extras"))
                && !node.value(QStringLiteral("extras")).isObject()) {
            throw BuildError(QStringLiteral("invalid GLB node extras: %1")
                    .arg(assetId));
        }
        const QString name = node.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || name.size() > 128 || nodeNames.contains(name)
                || node.contains(QStringLiteral("camera"))) {
            throw BuildError(QStringLiteral("invalid GLB node contract: %1")
                    .arg(assetId));
        }
        nodeNames.insert(name);
        if (node.contains(QStringLiteral("matrix"))
                && !vectorsClose(
                    finiteVector(node.value(QStringLiteral("matrix")), 16,
                        QStringLiteral("node.matrix")),
                    {1, 0, 0, 0, 0, 1, 0, 0,
                     0, 0, 1, 0, 0, 0, 0, 1})) {
            throw BuildError(QStringLiteral("GLB node matrix is not applied: %1")
                    .arg(name));
        }
        if (node.contains(QStringLiteral("rotation"))
                && !vectorsClose(
                    finiteVector(node.value(QStringLiteral("rotation")), 4,
                        QStringLiteral("node.rotation")),
                    {0, 0, 0, 1})) {
            throw BuildError(QStringLiteral("GLB node rotation is not applied: %1")
                    .arg(name));
        }
        if (node.contains(QStringLiteral("scale"))
                && !vectorsClose(
                    finiteVector(node.value(QStringLiteral("scale")), 3,
                        QStringLiteral("node.scale")),
                    {1, 1, 1})) {
            throw BuildError(QStringLiteral("GLB node scale is not applied: %1")
                    .arg(name));
        }
        nodeTranslations.append(node.contains(QStringLiteral("translation"))
                ? finiteVector(node.value(QStringLiteral("translation")), 3,
                    QStringLiteral("node.translation"))
                : QList<double>({0, 0, 0}));
        const QJsonObject extras = node.value(QStringLiteral("extras")).toObject();
        if (name.startsWith(QStringLiteral("ROOT_"))) {
            ++rootMetadataNodes;
            if (extras.value(QStringLiteral("unit_meters")).toDouble(
                        std::numeric_limits<double>::quiet_NaN()) != 1.0
                    || extras.value(QStringLiteral("up_axis")).toString()
                        != QStringLiteral("+Y")
                    || extras.value(QStringLiteral("forward_axis")).toString()
                        != QStringLiteral("+Z")
                    || extras.value(QStringLiteral("physics_authority")).toString()
                        != QStringLiteral("external")) {
                throw BuildError(QStringLiteral("GLB root metadata mismatch: %1")
                        .arg(assetId));
            }
        }
        if (node.contains(QStringLiteral("mesh"))) {
            jsonInteger(node.value(QStringLiteral("mesh")),
                    0, std::max<qsizetype>(0, meshes.size() - 1),
                    QStringLiteral("node mesh"));
            if (meshes.isEmpty()) {
                throw BuildError(QStringLiteral("GLB node references no mesh: %1")
                        .arg(assetId));
            }
            if (extras.value(QStringLiteral("physics_authority")).toString()
                    != QStringLiteral("external")) {
                throw BuildError(QStringLiteral("GLB render node claims physics: %1")
                        .arg(name));
            }
        }
        for (const QJsonValue &child : node.value(
                QStringLiteral("children")).toArray()) {
            const int childIndex = static_cast<int>(jsonInteger(
                    child, 0, std::max<qsizetype>(0, nodes.size() - 1),
                    QStringLiteral("node child")));
            if (childIndex == nodeIndex || parents.contains(childIndex)) {
                throw BuildError(QStringLiteral("invalid GLB node parentage: %1")
                        .arg(assetId));
            }
            parents.insert(childIndex, nodeIndex);
        }
    }
    if (nodeNames != expectedNodes) {
        throw BuildError(QStringLiteral("GLB nodes do not match manifest: %1")
                .arg(assetId));
    }
    if (rootMetadataNodes != 1) {
        throw BuildError(QStringLiteral("GLB must contain one ROOT node: %1")
                .arg(assetId));
    }
    QList<QList<double>> worldTranslations;
    worldTranslations.reserve(nodes.size());
    for (int nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        QSet<int> visited;
        int current = nodeIndex;
        QList<double> world = nodeTranslations.at(nodeIndex);
        while (parents.contains(current)) {
            if (visited.contains(current)) {
                throw BuildError(QStringLiteral("GLB node graph has a cycle: %1")
                        .arg(assetId));
            }
            visited.insert(current);
            current = parents.value(current);
            for (int axis = 0; axis < 3; ++axis) {
                world[axis] += nodeTranslations.at(current).at(axis);
            }
        }
        worldTranslations.append(world);
        if (nodes.at(nodeIndex).toObject().contains(QStringLiteral("mesh"))
                && !vectorsClose(world, {0, 0, 0})) {
            throw BuildError(QStringLiteral("GLB render transform is not applied: %1")
                    .arg(assetId));
        }
    }
    const QJsonArray scenes = document.value(QStringLiteral("scenes")).toArray();
    if (scenes.isEmpty()) {
        throw BuildError(QStringLiteral("GLB has no scene: %1").arg(assetId));
    }
    const int defaultScene = static_cast<int>(jsonInteger(
            document.value(QStringLiteral("scene")),
            0, scenes.size() - 1, QStringLiteral("default scene")));
    Q_UNUSED(defaultScene)
    for (const QJsonValue &value : scenes) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid GLB scene: %1").arg(assetId));
        }
        const QJsonObject scene = value.toObject();
        requireArrayField(scene, QStringLiteral("nodes"));
        for (const QJsonValue &root : scene.value(
                QStringLiteral("nodes")).toArray()) {
            jsonInteger(root, 0, std::max<qsizetype>(0, nodes.size() - 1),
                    QStringLiteral("scene root node"));
        }
    }

    const QJsonArray accessors = document.value(
            QStringLiteral("accessors")).toArray();
    const QSet<qint64> componentTypes = {
        5120, 5121, 5122, 5123, 5125, 5126,
    };
    const QSet<QString> accessorTypes = {
        QStringLiteral("SCALAR"), QStringLiteral("VEC2"),
        QStringLiteral("VEC3"), QStringLiteral("VEC4"),
        QStringLiteral("MAT2"), QStringLiteral("MAT3"),
        QStringLiteral("MAT4"),
    };
    const QHash<qint64, qint64> componentBytes = {
        {5120, 1}, {5121, 1}, {5122, 2},
        {5123, 2}, {5125, 4}, {5126, 4},
    };
    const QHash<QString, qint64> typeComponents = {
        {QStringLiteral("SCALAR"), 1}, {QStringLiteral("VEC2"), 2},
        {QStringLiteral("VEC3"), 3}, {QStringLiteral("VEC4"), 4},
        {QStringLiteral("MAT2"), 4}, {QStringLiteral("MAT3"), 9},
        {QStringLiteral("MAT4"), 16},
    };
    for (const QJsonValue &value : accessors) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid GLB accessor: %1")
                    .arg(assetId));
        }
        const QJsonObject accessor = value.toObject();
        if (accessor.contains(QStringLiteral("sparse"))) {
            throw BuildError(QStringLiteral("sparse GLB accessors are not allowed: %1")
                    .arg(assetId));
        }
        if (!accessor.contains(QStringLiteral("bufferView"))
                || bufferViews.isEmpty()) {
            throw BuildError(QStringLiteral("GLB accessor lacks a buffer view: %1")
                    .arg(assetId));
        }
        const int bufferViewIndex = static_cast<int>(jsonInteger(
                accessor.value(QStringLiteral("bufferView")),
                0, bufferViews.size() - 1,
                QStringLiteral("accessor.bufferView")));
        const qint64 count = jsonInteger(
                accessor.value(QStringLiteral("count")),
                0, std::numeric_limits<int>::max(),
                QStringLiteral("accessor.count"));
        const qint64 componentType = jsonInteger(
                accessor.value(QStringLiteral("componentType")),
                0, 65535, QStringLiteral("accessor.componentType"));
        const QString accessorType = accessor.value(
                QStringLiteral("type")).toString();
        if (!componentTypes.contains(componentType)
                || !accessorTypes.contains(accessorType)) {
            throw BuildError(QStringLiteral("unsupported GLB accessor: %1")
                    .arg(assetId));
        }
        const QJsonObject view = bufferViews.at(bufferViewIndex).toObject();
        const qint64 viewLength = jsonInteger(
                view.value(QStringLiteral("byteLength")),
                0, declaredBufferBytes,
                QStringLiteral("bufferView.byteLength"));
        const qint64 elementBytes = componentBytes.value(componentType)
                * typeComponents.value(accessorType);
        const qint64 stride = view.contains(QStringLiteral("byteStride"))
                ? jsonInteger(view.value(QStringLiteral("byteStride")),
                    elementBytes, 252, QStringLiteral("bufferView.byteStride"))
                : elementBytes;
        const qint64 accessorOffset = accessor.contains(
                QStringLiteral("byteOffset"))
                ? jsonInteger(accessor.value(QStringLiteral("byteOffset")),
                    0, viewLength, QStringLiteral("accessor.byteOffset")) : 0;
        if (count > 0 && (count - 1) > (
                std::numeric_limits<qint64>::max() - accessorOffset
                    - elementBytes) / stride) {
            throw BuildError(QStringLiteral("GLB accessor size overflow: %1")
                    .arg(assetId));
        }
        const qint64 requiredBytes = count == 0 ? accessorOffset
                : accessorOffset + (count - 1) * stride + elementBytes;
        if (requiredBytes > viewLength) {
            throw BuildError(QStringLiteral("GLB accessor exceeds its buffer view: %1")
                    .arg(assetId));
        }
    }

    qint64 triangleCount = 0;
    QList<double> measuredMinimum = {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
    };
    QList<double> measuredMaximum = {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (const QJsonValue &value : meshes) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid GLB mesh: %1").arg(assetId));
        }
        const QJsonObject mesh = value.toObject();
        requireArrayField(mesh, QStringLiteral("primitives"), true);
        if (mesh.value(QStringLiteral("primitives")).toArray().isEmpty()) {
            throw BuildError(QStringLiteral("GLB mesh has no primitives: %1")
                    .arg(assetId));
        }
        for (const QJsonValue &primitiveValue : mesh.value(
                QStringLiteral("primitives")).toArray()) {
            const QJsonObject primitive = primitiveValue.toObject();
            if (!primitiveValue.isObject()
                    || primitive.value(QStringLiteral("mode")).toInt(4) != 4) {
                throw BuildError(QStringLiteral("non-triangle GLB primitive: %1")
                        .arg(assetId));
            }
            const QJsonObject attributes = primitive.value(
                    QStringLiteral("attributes")).toObject();
            if (!primitive.value(QStringLiteral("attributes")).isObject()) {
                throw BuildError(QStringLiteral("invalid GLB primitive attributes: %1")
                        .arg(assetId));
            }
            const qint64 positionIndex = jsonInteger(
                    attributes.value(QStringLiteral("POSITION")),
                    0, std::max<qsizetype>(0, accessors.size() - 1),
                    QStringLiteral("primitive POSITION"));
            if (accessors.isEmpty()) {
                throw BuildError(QStringLiteral("GLB primitive has no accessor: %1")
                        .arg(assetId));
            }
            for (auto attribute = attributes.constBegin();
                    attribute != attributes.constEnd(); ++attribute) {
                jsonInteger(attribute.value(), 0, accessors.size() - 1,
                        QStringLiteral("primitive attribute"));
            }
            const QJsonObject positionAccessor = accessors.at(
                    positionIndex).toObject();
            if (positionAccessor.value(QStringLiteral("type")).toString()
                        != QStringLiteral("VEC3")
                    || positionAccessor.value(QStringLiteral("componentType")).toInt()
                        != 5126) {
                throw BuildError(QStringLiteral("invalid GLB POSITION accessor: %1")
                        .arg(assetId));
            }
            const QList<double> positionMinimum = finiteVector(
                    positionAccessor.value(QStringLiteral("min")), 3,
                    QStringLiteral("POSITION.min"));
            const QList<double> positionMaximum = finiteVector(
                    positionAccessor.value(QStringLiteral("max")), 3,
                    QStringLiteral("POSITION.max"));
            for (int axis = 0; axis < 3; ++axis) {
                if (positionMinimum.at(axis) > positionMaximum.at(axis)) {
                    throw BuildError(QStringLiteral("inverted GLB POSITION bounds: %1")
                            .arg(assetId));
                }
                measuredMinimum[axis] = std::min(
                        measuredMinimum.at(axis), positionMinimum.at(axis));
                measuredMaximum[axis] = std::max(
                        measuredMaximum.at(axis), positionMaximum.at(axis));
            }
            if (primitive.contains(QStringLiteral("material"))) {
                jsonInteger(primitive.value(QStringLiteral("material")),
                        0, std::max<qsizetype>(0,
                            document.value(QStringLiteral("materials")).toArray().size() - 1),
                        QStringLiteral("primitive material"));
                if (document.value(QStringLiteral("materials")).toArray().isEmpty()) {
                    throw BuildError(QStringLiteral("GLB primitive references no material: %1")
                            .arg(assetId));
                }
            }
            const qint64 triangleAccessor = primitive.contains(
                    QStringLiteral("indices"))
                    ? jsonInteger(primitive.value(QStringLiteral("indices")),
                        0, accessors.size() - 1,
                        QStringLiteral("primitive indices"))
                    : positionIndex;
            const qint64 count = jsonInteger(
                    accessors.at(triangleAccessor).toObject().value(
                        QStringLiteral("count")),
                    0, std::numeric_limits<int>::max(),
                    QStringLiteral("triangle accessor count"));
            if (primitive.contains(QStringLiteral("indices"))) {
                const QJsonObject indexAccessor = accessors.at(
                        triangleAccessor).toObject();
                const int componentType = indexAccessor.value(
                        QStringLiteral("componentType")).toInt();
                if (indexAccessor.value(QStringLiteral("type")).toString()
                            != QStringLiteral("SCALAR")
                        || (componentType != 5121 && componentType != 5123
                            && componentType != 5125)) {
                    throw BuildError(QStringLiteral("invalid GLB index accessor: %1")
                            .arg(assetId));
                }
            }
            if (count % 3 != 0) {
                throw BuildError(QStringLiteral("invalid triangle count: %1")
                        .arg(assetId));
            }
            triangleCount += count / 3;
            if (triangleCount > std::numeric_limits<int>::max()) {
                throw BuildError(QStringLiteral("GLB triangle count overflow: %1")
                        .arg(assetId));
            }
        }
    }
    if (triangleCount != jsonInteger(
                technical.value(QStringLiteral("trianglesLod0")),
                0, std::numeric_limits<int>::max(),
                QStringLiteral("technical.trianglesLod0"))
            || triangleCount > jsonInteger(
                budgets.value(QStringLiteral("maxTrianglesLod0")),
                0, std::numeric_limits<int>::max(),
                QStringLiteral("budgets.maxTrianglesLod0"))) {
        throw BuildError(QStringLiteral("GLB triangle budget mismatch: %1")
                .arg(assetId));
    }
    const QJsonObject bounds = technical.value(
            QStringLiteral("boundsMeters")).toObject();
    if (!technical.value(QStringLiteral("boundsMeters")).isObject()
            || !vectorsClose(measuredMinimum,
                finiteVector(bounds.value(QStringLiteral("minimum")), 3,
                    QStringLiteral("boundsMeters.minimum")))
            || !vectorsClose(measuredMaximum,
                finiteVector(bounds.value(QStringLiteral("maximum")), 3,
                    QStringLiteral("boundsMeters.maximum")))) {
        throw BuildError(QStringLiteral("GLB bounds do not match manifest: %1")
                .arg(assetId));
    }

    const QJsonArray materials = document.value(QStringLiteral("materials")).toArray();
    if (materials.size() != jsonInteger(
                technical.value(QStringLiteral("materials")),
                0, 4096, QStringLiteral("technical.materials"))
            || materials.size() > jsonInteger(
                budgets.value(QStringLiteral("maxMaterials")),
                0, 4096, QStringLiteral("budgets.maxMaterials"))) {
        throw BuildError(QStringLiteral("GLB material budget mismatch: %1")
                .arg(assetId));
    }
    QSet<QString> materialNames;
    for (const QJsonValue &value : materials) {
        const QString name = value.toObject().value(
                QStringLiteral("name")).toString();
        if (!value.isObject() || name.isEmpty() || name.size() > 128
                || materialNames.contains(name)) {
            throw BuildError(QStringLiteral("invalid GLB material names: %1")
                    .arg(assetId));
        }
        materialNames.insert(name);
    }

    const QJsonArray animations = document.value(
            QStringLiteral("animations")).toArray();
    QStringList animationNames;
    for (const QJsonValue &value : animations) {
        if (!value.isObject() || !value.toObject().value(
                QStringLiteral("name")).isString()) {
            throw BuildError(QStringLiteral("invalid GLB animation: %1")
                    .arg(assetId));
        }
        animationNames.append(value.toObject().value(
                QStringLiteral("name")).toString());
    }
    QStringList expectedAnimations;
    for (const QJsonValue &value : technical.value(
            QStringLiteral("animations")).toArray()) {
        if (!value.isString()) {
            throw BuildError(QStringLiteral("invalid animation manifest: %1")
                    .arg(assetId));
        }
        expectedAnimations.append(value.toString());
    }
    if (animationNames != expectedAnimations) {
        throw BuildError(QStringLiteral("GLB animations do not match manifest: %1")
                .arg(assetId));
    }

    requireArrayField(technical, QStringLiteral("sockets"));
    QHash<QString, int> nodeIndices;
    for (int index = 0; index < nodes.size(); ++index) {
        nodeIndices.insert(nodes.at(index).toObject().value(
                QStringLiteral("name")).toString(), index);
    }
    for (const QJsonValue &value : technical.value(
            QStringLiteral("sockets")).toArray()) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid socket manifest: %1")
                    .arg(assetId));
        }
        const QJsonObject socket = value.toObject();
        requireObjectShape(socket,
                {QStringLiteral("name"), QStringLiteral("positionMeters"),
                 QStringLiteral("halfWidthMeters")},
                {QStringLiteral("name"), QStringLiteral("positionMeters"),
                 QStringLiteral("halfWidthMeters")},
                QStringLiteral("socket"));
        const QString name = requiredString(
                socket, QStringLiteral("name"), QStringLiteral("socket"), 128);
        const QList<double> expectedPosition = finiteVector(
                socket.value(QStringLiteral("positionMeters")), 3,
                QStringLiteral("socket.positionMeters"));
        const double expectedWidth = socket.value(
                QStringLiteral("halfWidthMeters")).toDouble(
                    std::numeric_limits<double>::quiet_NaN());
        if (!nodeIndices.contains(name) || !std::isfinite(expectedWidth)
                || expectedWidth <= 0.0
                || !vectorsClose(worldTranslations.at(nodeIndices.value(name)),
                    expectedPosition)) {
            throw BuildError(QStringLiteral("GLB socket contract mismatch: %1")
                    .arg(name));
        }
        const QJsonValue actualWidth = nodes.at(nodeIndices.value(name)).toObject()
                .value(QStringLiteral("extras")).toObject().value(
                    QStringLiteral("socket_half_width_m"));
        if (!actualWidth.isDouble()
                || !std::isfinite(actualWidth.toDouble())
                || std::abs(actualWidth.toDouble() - expectedWidth) > 1e-5) {
            throw BuildError(QStringLiteral("GLB socket width mismatch: %1")
                    .arg(name));
        }
    }

    if (manifest.contains(QStringLiteral("physics"))) {
        if (!manifest.value(QStringLiteral("physics")).isObject()) {
            throw BuildError(QStringLiteral("invalid physics contract: %1")
                    .arg(assetId));
        }
        const QJsonObject physics = manifest.value(
                QStringLiteral("physics")).toObject();
        requireObjectShape(physics,
                {QStringLiteral("authority"), QStringLiteral("interaction"),
                 QStringLiteral("collisionProxy")},
                {QStringLiteral("authority"), QStringLiteral("interaction"),
                 QStringLiteral("surface"), QStringLiteral("collisionProxy")},
                QStringLiteral("physics"));
        const QString interaction = physics.value(
                QStringLiteral("interaction")).toString();
        if (physics.value(QStringLiteral("authority")).toString()
                    != QStringLiteral("external")
                || !QSet<QString>({QStringLiteral("visual-only"),
                                   QStringLiteral("surface"),
                                   QStringLiteral("obstacle"),
                                   QStringLiteral("rideable-feature")})
                        .contains(interaction)
                || !physics.value(QStringLiteral("collisionProxy")).isObject()) {
            throw BuildError(QStringLiteral("invalid physics authority: %1")
                    .arg(assetId));
        }
        const QJsonObject proxy = physics.value(
                QStringLiteral("collisionProxy")).toObject();
        requireObjectShape(proxy, {QStringLiteral("kind")},
                {QStringLiteral("kind"), QStringLiteral("node")},
                QStringLiteral("collision proxy"));
        const QString proxyKind = proxy.value(QStringLiteral("kind")).toString();
        if ((proxyKind != QStringLiteral("none")
                && proxyKind != QStringLiteral("node"))
                || (proxyKind == QStringLiteral("none")
                    && proxy.contains(QStringLiteral("node")))
                || (proxyKind == QStringLiteral("node")
                    && !nodeIndices.contains(proxy.value(
                        QStringLiteral("node")).toString()))) {
            throw BuildError(QStringLiteral("invalid physics collision proxy: %1")
                    .arg(assetId));
        }
        const bool visualOnly = interaction == QStringLiteral("visual-only");
        if (visualOnly != !physics.contains(QStringLiteral("surface"))
                || (visualOnly && proxyKind != QStringLiteral("none"))) {
            throw BuildError(QStringLiteral("invalid physics interaction: %1")
                    .arg(assetId));
        }
        if (!visualOnly) {
            if (!physics.value(QStringLiteral("surface")).isObject()) {
                throw BuildError(QStringLiteral("missing physics surface: %1")
                        .arg(assetId));
            }
            const QJsonObject surface = physics.value(
                    QStringLiteral("surface")).toObject();
            requireObjectShape(surface,
                    {QStringLiteral("coulombFriction"),
                     QStringLiteral("restitution")},
                    {QStringLiteral("coulombFriction"),
                     QStringLiteral("restitution")},
                    QStringLiteral("physics surface"));
            const double friction = surface.value(
                    QStringLiteral("coulombFriction")).toDouble(-1.0);
            const double restitution = surface.value(
                    QStringLiteral("restitution")).toDouble(-1.0);
            if (!surface.value(QStringLiteral("coulombFriction")).isDouble()
                    || !surface.value(QStringLiteral("restitution")).isDouble()
                    || !std::isfinite(friction) || friction < 0.0 || friction > 2.0
                    || !std::isfinite(restitution)
                    || restitution < 0.0 || restitution > 0.25) {
                throw BuildError(QStringLiteral("invalid physics surface values: %1")
                        .arg(assetId));
            }
        }
    }
}

void applyMaterialOverrides(
        ParsedAsset &asset,
        const QJsonArray &overrides)
{
    if (overrides.size() > 32) {
        throw BuildError(QStringLiteral("too many material overrides: %1")
                .arg(asset.assetId));
    }
    QJsonArray materials = asset.glbDocument.value(
            QStringLiteral("materials")).toArray();
    QHash<QString, int> materialIndices;
    for (int index = 0; index < materials.size(); ++index) {
        const QString name = materials.at(index).toObject().value(
                QStringLiteral("name")).toString();
        if (name.isEmpty() || materialIndices.contains(name)) {
            throw BuildError(QStringLiteral("GLB material names are not unique"));
        }
        materialIndices.insert(name, index);
    }

    QSet<QString> overrideNames;
    const QSet<QString> allowedKeys = {
        QStringLiteral("materialName"),
        QStringLiteral("baseColorSrgb"),
        QStringLiteral("roughness"),
        QStringLiteral("metallic"),
    };
    for (const QJsonValue &value : overrides) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid material override: %1")
                    .arg(asset.assetId));
        }
        const QJsonObject override = value.toObject();
        for (auto field = override.constBegin(); field != override.constEnd();
                ++field) {
            if (!allowedKeys.contains(field.key())) {
                throw BuildError(QStringLiteral("unknown material override field: %1")
                        .arg(field.key()));
            }
        }
        const QString name = override.value(
                QStringLiteral("materialName")).toString();
        if (name.isEmpty() || name.size() > 128
                || overrideNames.contains(name)
                || !materialIndices.contains(name)) {
            throw BuildError(QStringLiteral("unknown GLB material: %1").arg(name));
        }
        overrideNames.insert(name);
        const QString color = override.value(
                QStringLiteral("baseColorSrgb")).toString();
        if (!override.value(QStringLiteral("roughness")).isDouble()
                || !override.value(QStringLiteral("metallic")).isDouble()) {
            throw BuildError(QStringLiteral("invalid material factors: %1")
                    .arg(name));
        }
        const double roughness = override.value(
                QStringLiteral("roughness")).toDouble();
        const double metallic = override.value(
                QStringLiteral("metallic")).toDouble();
        if (!std::isfinite(roughness) || roughness < 0.0 || roughness > 1.0
                || !std::isfinite(metallic)
                || metallic < 0.0 || metallic > 1.0) {
            throw BuildError(QStringLiteral("invalid material factors: %1")
                    .arg(name));
        }

        const int index = materialIndices.value(name);
        QJsonObject material = materials.at(index).toObject();
        QJsonObject pbr = material.value(
                QStringLiteral("pbrMetallicRoughness")).toObject();
        const QJsonArray previousColor = pbr.value(
                QStringLiteral("baseColorFactor")).toArray();
        const double alpha = previousColor.size() >= 4
                ? previousColor.at(3).toDouble(1.0) : 1.0;
        pbr.insert(QStringLiteral("baseColorFactor"), linearColor(color, alpha));
        pbr.insert(QStringLiteral("roughnessFactor"), roughness);
        pbr.insert(QStringLiteral("metallicFactor"), metallic);
        material.insert(QStringLiteral("pbrMetallicRoughness"), pbr);
        materials[index] = material;

        QVariantMap properties;
        properties.insert(QStringLiteral("baseColor"), color);
        properties.insert(QStringLiteral("roughness"), roughness);
        properties.insert(QStringLiteral("metallic"), metallic);
        asset.materials.insert(name, properties);
    }
    asset.glbDocument.insert(QStringLiteral("materials"), materials);
}

ParsedAsset parseAsset(
        const QString &workspace,
        qulonglong workspaceDevice,
        qulonglong workspaceInode,
        const QString &manifestName)
{
    ParsedAsset result;
    const QString manifestRelative = ManifestRelativeDirectory
            + QLatin1Char('/') + manifestName;
    const QJsonObject manifest = parseJsonObject(
            readRepositoryFile(
                workspace,
                workspaceDevice,
                workspaceInode,
                manifestRelative,
                ManifestRelativeDirectory,
                MaximumManifestBytes,
                QStringLiteral("asset manifest")),
            manifestRelative);
    validateManifestContract(manifest);
    if ((manifest.contains(QStringLiteral("materialOverrides"))
                && !manifest.value(QStringLiteral("materialOverrides")).isArray())
            || (manifest.contains(QStringLiteral("runtimeVariants"))
                && !manifest.value(QStringLiteral("runtimeVariants")).isArray())) {
        throw BuildError(QStringLiteral("invalid manifest structure: %1")
                .arg(manifestRelative));
    }
    result.assetId = manifest.value(QStringLiteral("assetId")).toString();
    static const QRegularExpression assetIdPattern(
            QStringLiteral("^[A-Z]{2}-[0-9]{2}(?:-[a-z0-9-]+)?$"));
    if (!assetIdPattern.match(result.assetId).hasMatch()) {
        throw BuildError(QStringLiteral("invalid asset id in %1")
                .arg(manifestRelative));
    }

    QStringList glbPaths;
    QSet<QString> filePaths;
    const QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    if (files.isEmpty() || files.size() > 128) {
        throw BuildError(QStringLiteral("invalid manifest file inventory: %1")
                .arg(result.assetId));
    }
    for (const QJsonValue &value : files) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid manifest file entry: %1")
                    .arg(result.assetId));
        }
        const QJsonObject file = value.toObject();
        requireObjectShape(file,
                {QStringLiteral("path"), QStringLiteral("purpose")},
                {QStringLiteral("path"), QStringLiteral("purpose")},
                QStringLiteral("file inventory"));
        const QString path = file.value(
                QStringLiteral("path")).toString();
        const QString purpose = file.value(
                QStringLiteral("purpose")).toString();
        const QSet<QString> purposes = {
            QStringLiteral("source"), QStringLiteral("runtime"),
            QStringLiteral("intermediate"), QStringLiteral("texture"),
            QStringLiteral("license"), QStringLiteral("reference"),
            QStringLiteral("preview"),
        };
        if (!file.value(QStringLiteral("path")).isString()
                || !file.value(QStringLiteral("purpose")).isString()
                || path.isEmpty() || !purposes.contains(purpose)
                || filePaths.contains(path)) {
            throw BuildError(QStringLiteral("duplicate or empty asset path: %1")
                    .arg(path));
        }
        filePaths.insert(path);
        if (path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) {
            glbPaths.append(path);
        }
    }
    if (glbPaths.isEmpty()) {
        return result;
    }
    if (glbPaths.size() != 1) {
        throw BuildError(QStringLiteral("asset must reference one GLB: %1")
                .arg(result.assetId));
    }
    const QString glbRelative = glbPaths.first();
    result.glb = readRepositoryFile(
            workspace,
            workspaceDevice,
            workspaceInode,
            glbRelative,
            GeneratedRelativeDirectory,
            MaximumGlbBytes,
            QStringLiteral("canonical GLB"));
    if (result.glb.size() < 20
            || readLittleEndian32(result.glb, 0) != GlbMagic
            || readLittleEndian32(result.glb, 4) != GlbVersion
            || readLittleEndian32(result.glb, 8)
                != static_cast<quint32>(result.glb.size())) {
        throw BuildError(QStringLiteral("invalid GLB header: %1")
                .arg(glbRelative));
    }
    const quint32 jsonLength = readLittleEndian32(result.glb, 12);
    if (readLittleEndian32(result.glb, 16) != JsonChunk
            || jsonLength % 4 != 0
            || 20 + static_cast<qsizetype>(jsonLength) > result.glb.size()) {
        throw BuildError(QStringLiteral("invalid GLB JSON chunk: %1")
                .arg(glbRelative));
    }
    result.glbTailOffset = 20 + static_cast<qsizetype>(jsonLength);
    qsizetype offset = result.glbTailOffset;
    int binaryChunks = 0;
    while (offset < result.glb.size()) {
        if (offset + 8 > result.glb.size()) {
            throw BuildError(QStringLiteral("truncated GLB chunk: %1")
                    .arg(glbRelative));
        }
        const quint32 chunkLength = readLittleEndian32(result.glb, offset);
        const quint32 chunkType = readLittleEndian32(result.glb, offset + 4);
        if (chunkLength % 4 != 0 || chunkType != BinaryChunk
                || offset + 8 + static_cast<qsizetype>(chunkLength)
                    > result.glb.size() || ++binaryChunks > 1) {
            throw BuildError(QStringLiteral("invalid GLB binary chunk: %1")
                    .arg(glbRelative));
        }
        result.binaryChunkBytes = chunkLength;
        offset += 8 + static_cast<qsizetype>(chunkLength);
    }
    result.glbDocument = parseJsonObject(
            result.glb.mid(20, jsonLength).trimmed(), glbRelative);
    validateGlbDocument(
            result.glbDocument,
            result.glb.size(),
            result.binaryChunkBytes,
            manifest,
            result.assetId);
    applyMaterialOverrides(result,
            manifest.contains(QStringLiteral("materialOverrides"))
                ? manifest.value(QStringLiteral("materialOverrides")).toArray()
                : QJsonArray());

    QSet<QString> variantKeys;
    const QJsonArray variants = manifest.contains(
            QStringLiteral("runtimeVariants"))
            ? manifest.value(QStringLiteral("runtimeVariants")).toArray()
            : QJsonArray();
    if (variants.size() > MaximumRuntimeVariants) {
        throw BuildError(QStringLiteral("too many runtime variants: %1")
                .arg(result.assetId));
    }
    static const QRegularExpression variantKeyPattern(
            QStringLiteral("^[a-zA-Z0-9_-]{1,32}$"));
    QSet<QString> knownNodeNames;
    for (const QJsonValue &node : result.glbDocument.value(
            QStringLiteral("nodes")).toArray()) {
        knownNodeNames.insert(node.toObject().value(
                QStringLiteral("name")).toString());
    }
    for (const QJsonValue &value : variants) {
        if (!value.isObject()) {
            throw BuildError(QStringLiteral("invalid runtime variant: %1")
                    .arg(result.assetId));
        }
        const QJsonObject variant = value.toObject();
        if (variant.size() != 2
                || !variant.value(QStringLiteral("key")).isString()
                || !variant.value(QStringLiteral("rootNodes")).isArray()) {
            throw BuildError(QStringLiteral("invalid runtime variant shape: %1")
                    .arg(result.assetId));
        }
        const QString key = variant.value(QStringLiteral("key")).toString();
        const QJsonArray rootNodes = variant.value(
                QStringLiteral("rootNodes")).toArray();
        if (!variantKeyPattern.match(key).hasMatch()
                || rootNodes.isEmpty()
                || rootNodes.size() > MaximumVariantRootNodes
                || variantKeys.contains(key)) {
            throw BuildError(QStringLiteral("invalid runtime variant: %1")
                    .arg(result.assetId));
        }
        QStringList nodes;
        QSet<QString> seenNodes;
        for (const QJsonValue &node : rootNodes) {
            const QString name = node.toString();
            if (!node.isString() || name.isEmpty() || name.size() > 128
                    || seenNodes.contains(name)
                    || !knownNodeNames.contains(name)) {
                throw BuildError(QStringLiteral("invalid runtime variant node: %1")
                        .arg(name));
            }
            seenNodes.insert(name);
            nodes.append(name);
        }
        variantKeys.insert(key);
        result.variants.insert(key, nodes);
    }
    return result;
}

QJsonObject selectRootNodes(
        const ParsedAsset &asset,
        const QStringList &nodeNames)
{
    QJsonObject document = asset.glbDocument;
    if (nodeNames.isEmpty()) {
        return document;
    }
    const QJsonArray nodes = document.value(QStringLiteral("nodes")).toArray();
    QHash<QString, int> indices;
    for (int index = 0; index < nodes.size(); ++index) {
        const QString name = nodes.at(index).toObject().value(
                QStringLiteral("name")).toString();
        if (!name.isEmpty() && indices.contains(name)) {
            throw BuildError(QStringLiteral("GLB node names are not unique: %1")
                    .arg(asset.assetId));
        }
        if (!name.isEmpty()) indices.insert(name, index);
    }
    QJsonArray selected;
    for (const QString &name : nodeNames) {
        if (!indices.contains(name)) {
            throw BuildError(QStringLiteral("runtime variant names unknown node: %1")
                    .arg(name));
        }
        selected.append(indices.value(name));
    }
    QJsonArray scenes = document.value(QStringLiteral("scenes")).toArray();
    int sceneIndex = document.value(QStringLiteral("scene")).toInt(0);
    if (scenes.isEmpty()) {
        scenes.append(QJsonObject());
        sceneIndex = 0;
    }
    if (sceneIndex < 0 || sceneIndex >= scenes.size()) {
        throw BuildError(QStringLiteral("GLB default scene is invalid: %1")
                .arg(asset.assetId));
    }
    QJsonObject scene = scenes.at(sceneIndex).toObject();
    scene.insert(QStringLiteral("nodes"), selected);
    scenes[sceneIndex] = scene;
    document.insert(QStringLiteral("scenes"), scenes);
    document.insert(QStringLiteral("scene"), sceneIndex);
    return document;
}

QByteArray materializeGlb(
        const ParsedAsset &asset,
        const QStringList &nodeNames)
{
    QByteArray json = QJsonDocument(
            selectRootNodes(asset, nodeNames)).toJson(QJsonDocument::Compact);
    while (json.size() % 4 != 0) json.append(' ');
    const QByteArray tail = asset.glb.mid(asset.glbTailOffset);
    const qsizetype total = 12 + 8 + json.size() + tail.size();
    if (total > MaximumGlbBytes || total > std::numeric_limits<quint32>::max()) {
        throw BuildError(QStringLiteral("materialized GLB is too large: %1")
                .arg(asset.assetId));
    }
    QByteArray output;
    output.reserve(total);
    appendLittleEndian32(output, GlbMagic);
    appendLittleEndian32(output, GlbVersion);
    appendLittleEndian32(output, static_cast<quint32>(total));
    appendLittleEndian32(output, static_cast<quint32>(json.size()));
    appendLittleEndian32(output, JsonChunk);
    output.append(json);
    output.append(tail);
    return output;
}

QUrl writeMaterializedGlb(
        const QString &cacheRoot,
        const ParsedAsset &asset,
        const QString &variantKey,
        const QStringList &nodeNames,
        qulonglong generation)
{
    const QByteArray variantHash = QCryptographicHash::hash(
            variantKey.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    const QString path = QDir(cacheRoot).filePath(
            QStringLiteral("%1-%2-%3.glb")
                .arg(asset.assetId,
                     QString::fromLatin1(variantHash),
                     QString::number(generation)));
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        throw BuildError(QStringLiteral("cannot create development GLB: %1")
                .arg(path));
    }
    const QByteArray bytes = materializeGlb(asset, nodeNames);
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        throw BuildError(QStringLiteral("cannot commit development GLB: %1")
                .arg(path));
    }
    return QUrl::fromLocalFile(path);
}

} // namespace

WorkoutGameDevelopmentAssets::WorkoutGameDevelopmentAssets(QObject *parent) :
    QObject(parent)
{
    configureWorkspace();
    if (!developmentEnabled) return;
    configureWatcher();
    scheduleReload();
}

WorkoutGameDevelopmentAssets::~WorkoutGameDevelopmentAssets()
{
    if (buildWatcher.isRunning()) buildWatcher.waitForFinished();
}

QUrl WorkoutGameDevelopmentAssets::sourceForAsset(
        const QString &assetId,
        const QString &variantKey,
        qulonglong observedRevision) const
{
    Q_UNUSED(observedRevision)
    return publishedSources.value(sourceKey(assetId, variantKey));
}

QVariantMap WorkoutGameDevelopmentAssets::materialProperties(
        const QString &assetId,
        const QString &materialName,
        const QString &fallbackColor,
        double fallbackRoughness,
        double fallbackMetallic,
        qulonglong observedRevision) const
{
    Q_UNUSED(observedRevision)
    QVariantMap result;
    result.insert(QStringLiteral("baseColor"), fallbackColor);
    result.insert(QStringLiteral("roughness"), fallbackRoughness);
    result.insert(QStringLiteral("metallic"), fallbackMetallic);
    const auto asset = publishedMaterials.constFind(assetId);
    if (asset != publishedMaterials.constEnd()) {
        const auto material = asset->constFind(materialName);
        if (material != asset->constEnd()) result = material.value();
    }
    return result;
}

void WorkoutGameDevelopmentAssets::reportRuntimeError(
        const QString &assetId,
        const QString &message)
{
    qWarning().noquote()
            << "Workout Game development asset load failed:"
            << assetId << message;
}

void WorkoutGameDevelopmentAssets::scheduleReload()
{
    if (!developmentEnabled) return;
    if (buildWatcher.isRunning()) {
        reloadRequestedDuringBuild = true;
        return;
    }
    reloadTimer->start();
}

void WorkoutGameDevelopmentAssets::startReload()
{
    if (buildWatcher.isRunning()) {
        reloadRequestedDuringBuild = true;
        return;
    }
    const qulonglong generation = nextGeneration++;
    buildWatcher.setFuture(QtConcurrent::run(
            &WorkoutGameDevelopmentAssets::buildAssets,
            workspaceRoot,
            cacheDirectory.path(),
            generation,
            workspaceDevice,
            workspaceInode));
}

void WorkoutGameDevelopmentAssets::finishReload()
{
    const BuildResult result = buildWatcher.result();
    if (result.error.isEmpty()) {
        retiredCacheFiles.unite(publishedCacheFiles);
        publishedCacheFiles = result.cacheFiles;
        for (const QString &current : publishedCacheFiles) {
            retiredCacheFiles.remove(current);
        }
        publishedSources = result.sources;
        publishedMaterials = result.materials;
        publishedError.clear();
        ++publishedRevision;
        emit assetsChanged();
        if (!retiredCacheFiles.isEmpty() && !cacheCleanupTimer->isActive()) {
            cacheCleanupTimer->start();
        }
    } else {
        publishedError = result.error;
        qWarning().noquote()
                << "Workout Game development assets rejected:"
                << publishedError;
        emit assetsChanged();
    }
    refreshWatchedFiles();
    if (reloadRequestedDuringBuild) {
        reloadRequestedDuringBuild = false;
        reloadTimer->start();
    }
}

WorkoutGameDevelopmentAssets::BuildResult
WorkoutGameDevelopmentAssets::buildAssets(
        const QString &workspaceRoot,
        const QString &cacheRoot,
        qulonglong generation,
        qulonglong workspaceDevice,
        qulonglong workspaceInode)
{
    BuildResult result;
    try {
        const QString manifestDirectory = canonicalDirectory(
                QDir(workspaceRoot).filePath(ManifestRelativeDirectory),
                QStringLiteral("asset manifest directory"));
        canonicalDirectory(
                QDir(workspaceRoot).filePath(GeneratedRelativeDirectory),
                QStringLiteral("generated asset directory"));
        QDir manifests(manifestDirectory);
        const QStringList files = manifests.entryList(
                QStringList() << QStringLiteral("*.json"),
                QDir::Files | QDir::NoSymLinks,
                QDir::Name);
        if (files.isEmpty()) {
            throw BuildError(QStringLiteral("no development asset manifests"));
        }
        QSet<QString> assetIds;
        for (const QString &file : files) {
            const ParsedAsset asset = parseAsset(
                    workspaceRoot,
                    workspaceDevice,
                    workspaceInode,
                    file);
            if (asset.glb.isEmpty()) continue;
            if (assetIds.contains(asset.assetId)) {
                throw BuildError(QStringLiteral("duplicate asset id: %1")
                        .arg(asset.assetId));
            }
            assetIds.insert(asset.assetId);
            result.materials.insert(asset.assetId, asset.materials);
            const QUrl complete = writeMaterializedGlb(
                    cacheRoot, asset, QString(), QStringList(), generation);
            result.sources.insert(
                    sourceKey(asset.assetId, QString()), complete);
            result.cacheFiles.insert(complete.toLocalFile());
            for (auto variant = asset.variants.constBegin();
                    variant != asset.variants.constEnd(); ++variant) {
                const QUrl selected = writeMaterializedGlb(
                        cacheRoot,
                        asset,
                        variant.key(),
                        variant.value(),
                        generation);
                result.sources.insert(
                        sourceKey(asset.assetId, variant.key()), selected);
                result.cacheFiles.insert(selected.toLocalFile());
            }
        }
    } catch (const std::exception &error) {
        for (const QString &path : std::as_const(result.cacheFiles)) {
            QFile::remove(path);
        }
        result.sources.clear();
        result.materials.clear();
        result.cacheFiles.clear();
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

QString WorkoutGameDevelopmentAssets::sourceKey(
        const QString &assetId,
        const QString &variantKey)
{
    return assetId + QChar(0x1f) + variantKey;
}

void WorkoutGameDevelopmentAssets::configureWorkspace()
{
    const QString requested = QString::fromLocal8Bit(
            qgetenv("GC_WORKOUT_GAME_ASSET_WORKSPACE")).trimmed();
    if (requested.isEmpty()) return;
#if !defined(Q_OS_UNIX)
    publishedError = QStringLiteral(
            "development asset workspace requires secure Unix file access");
    qWarning().noquote()
            << "Workout Game development assets disabled:"
            << publishedError;
    return;
#else
    try {
        workspaceRoot = canonicalDirectory(
                requested, QStringLiteral("development asset workspace"));
        struct stat identity{};
        if (::stat(QFile::encodeName(workspaceRoot).constData(), &identity) != 0
                || !S_ISDIR(identity.st_mode)) {
            throw BuildError(QStringLiteral(
                    "cannot identify development asset workspace"));
        }
        workspaceDevice = static_cast<qulonglong>(identity.st_dev);
        workspaceInode = static_cast<qulonglong>(identity.st_ino);
        canonicalDirectory(
                QDir(workspaceRoot).filePath(ManifestRelativeDirectory),
                QStringLiteral("asset manifest directory"));
        canonicalDirectory(
                QDir(workspaceRoot).filePath(GeneratedRelativeDirectory),
                QStringLiteral("generated asset directory"));
        if (!cacheDirectory.isValid()) {
            throw BuildError(QStringLiteral("cannot create development asset cache"));
        }
        developmentEnabled = true;
    } catch (const std::exception &error) {
        workspaceRoot.clear();
        publishedError = QString::fromUtf8(error.what());
        qWarning().noquote()
                << "Workout Game development assets disabled:"
                << publishedError;
    }
#endif
}

void WorkoutGameDevelopmentAssets::configureWatcher()
{
    watcher = new QFileSystemWatcher(this);
    reloadTimer = new QTimer(this);
    cacheCleanupTimer = new QTimer(this);
    reloadTimer->setSingleShot(true);
    reloadTimer->setInterval(150);
    cacheCleanupTimer->setSingleShot(true);
    cacheCleanupTimer->setInterval(10000);
    connect(watcher, &QFileSystemWatcher::directoryChanged,
            this, &WorkoutGameDevelopmentAssets::scheduleReload);
    connect(watcher, &QFileSystemWatcher::fileChanged,
            this, &WorkoutGameDevelopmentAssets::scheduleReload);
    connect(reloadTimer, &QTimer::timeout,
            this, &WorkoutGameDevelopmentAssets::startReload);
    connect(&buildWatcher, &QFutureWatcher<BuildResult>::finished,
            this, &WorkoutGameDevelopmentAssets::finishReload);
    connect(cacheCleanupTimer, &QTimer::timeout, this, [this]() {
        QSet<QString> retry;
        for (const QString &path : std::as_const(retiredCacheFiles)) {
            if (!publishedCacheFiles.contains(path)
                    && QFile::exists(path) && !QFile::remove(path)) {
                retry.insert(path);
            }
        }
        retiredCacheFiles = retry;
        if (!retiredCacheFiles.isEmpty()) cacheCleanupTimer->start();
    });
    refreshWatchedFiles();
    const QString manifestDirectory = QDir(workspaceRoot).filePath(
            ManifestRelativeDirectory);
    const QString generatedDirectory = QDir(workspaceRoot).filePath(
            GeneratedRelativeDirectory);
    if (!watcher->directories().contains(manifestDirectory)
            || !watcher->directories().contains(generatedDirectory)) {
        developmentEnabled = false;
        publishedError = QStringLiteral(
                "cannot watch development asset directories");
        qWarning().noquote() << publishedError;
    }
}

void WorkoutGameDevelopmentAssets::refreshWatchedFiles()
{
    if (!watcher) return;
    const QString assetRoot = QDir(workspaceRoot).filePath(
            QStringLiteral("contrib/workout-game-assets"));
    const QStringList directoryCandidates = {
        workspaceRoot,
        QDir(workspaceRoot).filePath(QStringLiteral("contrib")),
        assetRoot,
        QDir(workspaceRoot).filePath(ManifestRelativeDirectory),
        QDir(workspaceRoot).filePath(GeneratedRelativeDirectory),
    };
    QStringList desiredDirectories;
    for (const QString &path : directoryCandidates) {
        const QFileInfo info(path);
        const QString absolute = QDir::cleanPath(info.absoluteFilePath());
        if (!info.isDir() || info.isSymLink()
                || info.canonicalFilePath() != absolute
                || !(absolute == workspaceRoot
                     || absolute.startsWith(workspaceRoot
                            + QDir::separator()))) {
            continue;
        }
        desiredDirectories.append(absolute);
    }
    // QFileSystemWatcher may retain a path after an atomic directory swap while
    // still watching the renamed inode. Rebind every development path after a
    // completed build so subsequent editor saves target the current objects.
    const QStringList currentDirectories = watcher->directories();
    if (!currentDirectories.isEmpty()) {
        watcher->removePaths(currentDirectories);
    }
    const QStringList rejectedDirectories = desiredDirectories.isEmpty()
            ? QStringList() : watcher->addPaths(desiredDirectories);
    if (!rejectedDirectories.isEmpty()) {
        qWarning().noquote()
                << "Workout Game development asset directories could not be watched:"
                << rejectedDirectories.join(QStringLiteral(", "));
    }

    QStringList desired;
    QDir manifests(QDir(workspaceRoot).filePath(ManifestRelativeDirectory));
    for (const QString &name : manifests.entryList(
            QStringList() << QStringLiteral("*.json"),
            QDir::Files | QDir::NoSymLinks,
            QDir::Name)) {
        desired.append(manifests.filePath(name));
    }
    QDir generated(QDir(workspaceRoot).filePath(GeneratedRelativeDirectory));
    for (const QString &name : generated.entryList(
            QStringList() << QStringLiteral("*.glb"),
            QDir::Files | QDir::NoSymLinks,
            QDir::Name)) {
        desired.append(generated.filePath(name));
    }
    const QStringList currentFiles = watcher->files();
    if (!currentFiles.isEmpty()) {
        watcher->removePaths(currentFiles);
    }
    const QStringList rejected = desired.isEmpty()
            ? QStringList() : watcher->addPaths(desired);
    if (!rejected.isEmpty()) {
        qWarning().noquote()
                << "Workout Game development asset files could not be watched:"
                << rejected.join(QStringLiteral(", "));
    }
}
