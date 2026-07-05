/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <qglobal.h>

#ifndef QT_NO_TEXTODFWRITER

#include "zipreader.h"
#include "zipwriter.h"
#include <qdatetime.h>
#include <qplatformdefs.h>
#include <qendian.h>
#include <qdebug.h>
#include <qdir.h>
#include <qhash.h>
#include <qsavefile.h>
#include <qset.h>

#include <algorithm>
#include <limits>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

#ifdef Q_CC_MSVC
#include <QtZlib\zlib.h>
#else
#include <zlib.h>
#endif


#if defined(Q_OS_WIN)
#  undef S_IFREG
#  define S_IFREG 0100000
#  ifndef S_IFDIR
#    define S_IFDIR 0040000
#  endif
#  ifndef S_ISDIR
#    define S_ISDIR(x) ((x) & S_IFDIR) > 0
#  endif
#  ifndef S_ISREG
#    define S_ISREG(x) ((x) & 0170000) == S_IFREG
#  endif
#  define S_IFLNK 020000
#  define S_ISLNK(x) ((x) & S_IFLNK) > 0
#  ifndef S_IRUSR
#    define S_IRUSR 0400
#  endif
#  ifndef S_IWUSR
#    define S_IWUSR 0200
#  endif
#  ifndef S_IXUSR
#    define S_IXUSR 0100
#  endif
#  define S_IRGRP 0040
#  define S_IWGRP 0020
#  define S_IXGRP 0010
#  define S_IROTH 0004
#  define S_IWOTH 0002
#  define S_IXOTH 0001
#endif

#if 0
#define ZDEBUG qDebug
#else
#define ZDEBUG if (0) qDebug
#endif

QT_BEGIN_NAMESPACE

static inline uint readUInt(const uchar *data)
{
    return (data[0]) + (data[1]<<8) + (data[2]<<16) + (data[3]<<24);
}

static inline ushort readUShort(const uchar *data)
{
    return (data[0]) + (data[1]<<8);
}

static inline void writeUInt(uchar *data, uint i)
{
    data[0] = i & 0xff;
    data[1] = (i>>8) & 0xff;
    data[2] = (i>>16) & 0xff;
    data[3] = (i>>24) & 0xff;
}

static inline void writeUShort(uchar *data, ushort i)
{
    data[0] = i & 0xff;
    data[1] = (i>>8) & 0xff;
}

static inline void copyUInt(uchar *dest, const uchar *src)
{
    dest[0] = src[0];
    dest[1] = src[1];
    dest[2] = src[2];
    dest[3] = src[3];
}

static inline void copyUShort(uchar *dest, const uchar *src)
{
    dest[0] = src[0];
    dest[1] = src[1];
}

static void writeMSDosDate(uchar *dest, const QDateTime& dt)
{
    if (dt.isValid()) {
        quint16 time =
            (dt.time().hour() << 11)    // 5 bit hour
            | (dt.time().minute() << 5)   // 6 bit minute
            | (dt.time().second() >> 1);  // 5 bit double seconds

        dest[0] = time & 0xff;
        dest[1] = time >> 8;

        quint16 date =
            ((dt.date().year() - 1980) << 9) // 7 bit year 1980-based
            | (dt.date().month() << 5)           // 4 bit month
            | (dt.date().day());                 // 5 bit day

        dest[2] = char(date);
        dest[3] = char(date >> 8);
    } else {
        dest[0] = 0;
        dest[1] = 0;
        dest[2] = 0;
        dest[3] = 0;
    }
}

static quint32 permissionsToMode(QFile::Permissions perms)
{
    quint32 mode = 0;
    if (perms & QFile::ReadOwner)
        mode |= S_IRUSR;
    if (perms & QFile::WriteOwner)
        mode |= S_IWUSR;
    if (perms & QFile::ExeOwner)
        mode |= S_IXUSR;
    if (perms & QFile::ReadUser)
        mode |= S_IRUSR;
    if (perms & QFile::WriteUser)
        mode |= S_IWUSR;
    if (perms & QFile::ExeUser)
        mode |= S_IXUSR;
    if (perms & QFile::ReadGroup)
        mode |= S_IRGRP;
    if (perms & QFile::WriteGroup)
        mode |= S_IWGRP;
    if (perms & QFile::ExeGroup)
        mode |= S_IXGRP;
    if (perms & QFile::ReadOther)
        mode |= S_IROTH;
    if (perms & QFile::WriteOther)
        mode |= S_IWOTH;
    if (perms & QFile::ExeOther)
        mode |= S_IXOTH;
    return mode;
}

static int inflate(Bytef *dest, ulong *destLen, const Bytef *source, ulong sourceLen)
{
    z_stream stream;
    int err;

    stream.next_in = (Bytef*)source;
    stream.avail_in = (uInt)sourceLen;
    if ((uLong)stream.avail_in != sourceLen)
        return Z_BUF_ERROR;

    stream.next_out = dest;
    stream.avail_out = (uInt)*destLen;
    if ((uLong)stream.avail_out != *destLen)
        return Z_BUF_ERROR;

    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;

    err = inflateInit2(&stream, -MAX_WBITS);
    if (err != Z_OK)
        return err;

    err = inflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        inflateEnd(&stream);
        if (err == Z_NEED_DICT || (err == Z_BUF_ERROR && stream.avail_in == 0))
            return Z_DATA_ERROR;
        return err;
    }
    *destLen = stream.total_out;

    err = inflateEnd(&stream);
    return err;
}

static int deflate (Bytef *dest, ulong *destLen, const Bytef *source, ulong sourceLen)
{
    z_stream stream;
    int err;

    stream.next_in = (Bytef*)source;
    stream.avail_in = (uInt)sourceLen;
    stream.next_out = dest;
    stream.avail_out = (uInt)*destLen;
    if ((uLong)stream.avail_out != *destLen) return Z_BUF_ERROR;

    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;
    stream.opaque = (voidpf)0;

    err = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (err != Z_OK) return err;

    err = deflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        deflateEnd(&stream);
        return err == Z_OK ? Z_BUF_ERROR : err;
    }
    *destLen = stream.total_out;

    err = deflateEnd(&stream);
    return err;
}

static QFile::Permissions modeToPermissions(quint32 mode)
{
    QFile::Permissions ret;
    if (mode & S_IRUSR)
        ret |= QFile::ReadOwner;
    if (mode & S_IWUSR)
        ret |= QFile::WriteOwner;
    if (mode & S_IXUSR)
        ret |= QFile::ExeOwner;
    if (mode & S_IRUSR)
        ret |= QFile::ReadUser;
    if (mode & S_IWUSR)
        ret |= QFile::WriteUser;
    if (mode & S_IXUSR)
        ret |= QFile::ExeUser;
    if (mode & S_IRGRP)
        ret |= QFile::ReadGroup;
    if (mode & S_IWGRP)
        ret |= QFile::WriteGroup;
    if (mode & S_IXGRP)
        ret |= QFile::ExeGroup;
    if (mode & S_IROTH)
        ret |= QFile::ReadOther;
    if (mode & S_IWOTH)
        ret |= QFile::WriteOther;
    if (mode & S_IXOTH)
        ret |= QFile::ExeOther;
    return ret;
}

static QDateTime readMSDosDate(const uchar *src)
{
    uint dosDate = readUInt(src);
    quint64 uDate;
    uDate = (quint64)(dosDate >> 16);
    uint tm_mday = (uDate & 0x1f);
    uint tm_mon =  ((uDate & 0x1E0) >> 5);
    uint tm_year = (((uDate & 0x0FE00) >> 9) + 1980);
    uint tm_hour = ((dosDate & 0xF800) >> 11);
    uint tm_min =  ((dosDate & 0x7E0) >> 5);
    uint tm_sec =  ((dosDate & 0x1f) << 1);

    return QDateTime(QDate(tm_year, tm_mon, tm_mday), QTime(tm_hour, tm_min, tm_sec));
}

struct LocalFileHeader
{
    uchar signature[4]; //  0x04034b50
    uchar version_needed[2];
    uchar general_purpose_bits[2];
    uchar compression_method[2];
    uchar last_mod_file[4];
    uchar crc_32[4];
    uchar compressed_size[4];
    uchar uncompressed_size[4];
    uchar file_name_length[2];
    uchar extra_field_length[2];
};

struct DataDescriptor
{
    uchar crc_32[4];
    uchar compressed_size[4];
    uchar uncompressed_size[4];
};

struct CentralFileHeader
{
    uchar signature[4]; // 0x02014b50
    uchar version_made[2];
    uchar version_needed[2];
    uchar general_purpose_bits[2];
    uchar compression_method[2];
    uchar last_mod_file[4];
    uchar crc_32[4];
    uchar compressed_size[4];
    uchar uncompressed_size[4];
    uchar file_name_length[2];
    uchar extra_field_length[2];
    uchar file_comment_length[2];
    uchar disk_start[2];
    uchar internal_file_attributes[2];
    uchar external_file_attributes[4];
    uchar offset_local_header[4];
    LocalFileHeader toLocalHeader() const;
};

struct EndOfDirectory
{
    uchar signature[4]; // 0x06054b50
    uchar this_disk[2];
    uchar start_of_directory_disk[2];
    uchar num_dir_entries_this_disk[2];
    uchar num_dir_entries[2];
    uchar directory_size[4];
    uchar dir_start_offset[4];
    uchar comment_length[2];
};

struct FileHeader
{
    CentralFileHeader h;
    QByteArray file_name;
    QByteArray extra_field;
    QByteArray file_comment;
};

ZipReader::FileInfo::FileInfo()
    : isDir(false), isFile(false), isSymLink(false), crc_32(0), size(0)
{
}

ZipReader::FileInfo::~FileInfo()
{
}

ZipReader::FileInfo::FileInfo(const FileInfo &other)
{
    operator=(other);
}

ZipReader::FileInfo& ZipReader::FileInfo::operator=(const FileInfo &other)
{
    filePath = other.filePath;
    isDir = other.isDir;
    isFile = other.isFile;
    isSymLink = other.isSymLink;
    permissions = other.permissions;
    crc_32 = other.crc_32;
    size = other.size;
    lastModified = other.lastModified;
    return *this;
}

bool ZipReader::FileInfo::isValid() const
{
    return isDir || isFile || isSymLink;
}

class QZipPrivate
{
public:
    QZipPrivate(QIODevice *device)
        : device(device), dirtyFileTree(true), start_of_directory(0)
    {
    }

    QZipPrivate(std::unique_ptr<QIODevice> dev)
        : device(dev.get()), ownedDevice(std::move(dev)), dirtyFileTree(true), start_of_directory(0)
    {
    }

    virtual ~QZipPrivate() = default;

    void fillFileInfo(int index, ZipReader::FileInfo &fileInfo) const;

    QIODevice *device;
    std::unique_ptr<QIODevice> ownedDevice;
    bool dirtyFileTree;
    QList<FileHeader> fileHeaders;
    QByteArray comment;
    uint start_of_directory;
};

void QZipPrivate::fillFileInfo(int index, ZipReader::FileInfo &fileInfo) const
{
    FileHeader header = fileHeaders.at(index);
    fileInfo.filePath = QString::fromLocal8Bit(header.file_name);
    const quint32 mode = (qFromLittleEndian<quint32>(&header.h.external_file_attributes[0]) >> 16) & 0xFFFF;
    fileInfo.isDir = S_ISDIR(mode);
    fileInfo.isFile = S_ISREG(mode);
    fileInfo.isSymLink = S_ISLNK(mode);
    fileInfo.permissions = modeToPermissions(mode);
    fileInfo.crc_32 = readUInt(header.h.crc_32);
    fileInfo.size = readUInt(header.h.uncompressed_size);
    fileInfo.lastModified = readMSDosDate(header.h.last_mod_file);
    // Files of size 0 ending in "/" seems to be an alternative directory encoding used by some compressors
    bool altIsDir = (fileInfo.size == 0 && fileInfo.filePath.endsWith("/"));
    fileInfo.isDir = fileInfo.isDir || altIsDir;
    fileInfo.isFile = fileInfo.isFile && !altIsDir;
}

class ZipReaderPrivate : public QZipPrivate
{
public:
    ZipReaderPrivate(QIODevice *device)
        : QZipPrivate(device), status(ZipReader::NoError)
    {
    }

    ZipReaderPrivate(std::unique_ptr<QIODevice> device)
        : QZipPrivate(std::move(device)), status(ZipReader::NoError)
    {
    }

    void scanFiles();

    ZipReader::Status status;
};

class ZipWriterPrivate : public QZipPrivate
{
public:
    ZipWriterPrivate(QIODevice *device)
        : QZipPrivate(device),
        status(ZipWriter::NoError),
        permissions(QFile::ReadOwner | QFile::WriteOwner),
        compressionPolicy(ZipWriter::AlwaysCompress)
    {
    }

    ZipWriterPrivate(std::unique_ptr<QIODevice> device)
        : QZipPrivate(std::move(device)),
        status(ZipWriter::NoError),
        permissions(QFile::ReadOwner | QFile::WriteOwner),
        compressionPolicy(ZipWriter::AlwaysCompress)
    {
    }

    ZipWriter::Status status;
    QFile::Permissions permissions;
    ZipWriter::CompressionPolicy compressionPolicy;

    enum EntryType { Directory, File, Symlink };

    void addEntry(EntryType type, const QString &fileName, const QByteArray &contents);
};

LocalFileHeader CentralFileHeader::toLocalHeader() const
{
    LocalFileHeader h;
    writeUInt(h.signature, 0x04034b50);
    copyUShort(h.version_needed, version_needed);
    copyUShort(h.general_purpose_bits, general_purpose_bits);
    copyUShort(h.compression_method, compression_method);
    copyUInt(h.last_mod_file, last_mod_file);
    copyUInt(h.crc_32, crc_32);
    copyUInt(h.compressed_size, compressed_size);
    copyUInt(h.uncompressed_size, uncompressed_size);
    copyUShort(h.file_name_length, file_name_length);
    copyUShort(h.extra_field_length, extra_field_length);
    return h;
}

void ZipReaderPrivate::scanFiles()
{
    if (!dirtyFileTree)
        return;

    fileHeaders.clear();
    comment.clear();

    const auto failRead = [this](const char *message) {
        if (message)
            qWarning() << message;
        status = ZipReader::FileReadError;
        fileHeaders.clear();
        comment.clear();
    };

    if (!(device->isOpen() || device->open(QIODevice::ReadOnly))) {
        status = ZipReader::FileOpenError;
        return;
    }

    if ((device->openMode() & QIODevice::ReadOnly) == 0) {
        status = ZipReader::FileReadError;
        return;
    }

    dirtyFileTree = false;
    status = ZipReader::NoError;

    uchar signature[4];
    if (!device->seek(0)
        || device->read(reinterpret_cast<char *>(signature), sizeof(signature))
            != sizeof(signature)
        || readUInt(signature) != 0x04034b50) {
        failRead("QZip: not a zip file");
        return;
    }

    int commentLengthFromEnd = 0;
    qint64 endOfDirectoryPosition = -1;
    EndOfDirectory eod;
    while (endOfDirectoryPosition < 0) {
        if (commentLengthFromEnd > 65535) {
            failRead("QZip: EndOfDirectory not found");
            return;
        }

        const qint64 position =
            device->size() - sizeof(EndOfDirectory) - commentLengthFromEnd;
        if (position < 0
            || !device->seek(position)
            || device->read(
                   reinterpret_cast<char *>(&eod),
                   sizeof(EndOfDirectory)) != sizeof(EndOfDirectory)) {
            failRead("QZip: EndOfDirectory not found");
            return;
        }
        if (readUInt(eod.signature) == 0x06054b50)
            endOfDirectoryPosition = position;
        else
            ++commentLengthFromEnd;
    }

    const int commentLength = readUShort(eod.comment_length);
    const int entryCount = readUShort(eod.num_dir_entries);
    const quint32 directoryOffset = readUInt(eod.dir_start_offset);
    const quint32 directorySize = readUInt(eod.directory_size);
    if (commentLength != commentLengthFromEnd
        || readUShort(eod.this_disk) != 0
        || readUShort(eod.start_of_directory_disk) != 0
        || readUShort(eod.num_dir_entries_this_disk) != entryCount
        || entryCount == 0xffff
        || directoryOffset == 0xffffffffU
        || directorySize == 0xffffffffU) {
        failRead("QZip: invalid EndOfDirectory record");
        return;
    }

    const qint64 directoryStart = directoryOffset;
    const qint64 directoryEnd = directoryStart + directorySize;
    if (directoryEnd < directoryStart
        || directoryEnd != endOfDirectoryPosition) {
        failRead("QZip: invalid central directory bounds");
        return;
    }

    if (commentLength > 0) {
        comment = device->read(commentLength);
        if (comment.size() != commentLength) {
            failRead("QZip: failed to read archive comment");
            return;
        }
    }

    if (!device->seek(directoryStart)) {
        failRead("QZip: failed to seek to central directory");
        return;
    }

    const auto readField =
        [this, directoryEnd](int length, QByteArray *target) {
            if (length < 0
                || device->pos() < 0
                || device->pos() > directoryEnd - length) {
                return false;
            }
            *target = device->read(length);
            return target->size() == length;
        };

    for (int index = 0; index < entryCount; ++index) {
        if (device->pos() < 0
            || device->pos() > directoryEnd
                - static_cast<qint64>(sizeof(CentralFileHeader))) {
            failRead("QZip: incomplete central directory header");
            return;
        }

        FileHeader header;
        if (device->read(
                reinterpret_cast<char *>(&header.h),
                sizeof(CentralFileHeader)) != sizeof(CentralFileHeader)
            || readUInt(header.h.signature) != 0x02014b50) {
            failRead("QZip: invalid central directory header");
            return;
        }

        if (!readField(
                readUShort(header.h.file_name_length),
                &header.file_name)
            || !readField(
                readUShort(header.h.extra_field_length),
                &header.extra_field)
            || !readField(
                readUShort(header.h.file_comment_length),
                &header.file_comment)) {
            failRead("QZip: incomplete central directory entry");
            return;
        }

        ZDEBUG("found file '%s'", header.file_name.data());
        fileHeaders.append(header);
    }

    if (device->pos() != directoryEnd) {
        failRead("QZip: central directory size does not match entry count");
        return;
    }
}

void ZipWriterPrivate::addEntry(EntryType type, const QString &fileName, const QByteArray &contents/*, QFile::Permissions permissions, QZip::Method m*/)
{
#ifndef NDEBUG
    static const char *entryTypes[] = {
        "directory",
        "file     ",
        "symlink  " };
    ZDEBUG() << "adding" << entryTypes[type] <<":" << fileName.toUtf8().data() << (type == 2 ? QByteArray(" -> " + contents).constData() : "");
#endif

    if (! (device->isOpen() || device->open(QIODevice::WriteOnly))) {
        status = ZipWriter::FileOpenError;
        return;
    }
    device->seek(start_of_directory);

    // don't compress small files
    ZipWriter::CompressionPolicy compression = compressionPolicy;
    if (compressionPolicy == ZipWriter::AutoCompress) {
        if (contents.length() < 64)
            compression = ZipWriter::NeverCompress;
        else
            compression = ZipWriter::AlwaysCompress;
    }

    FileHeader header;
    memset(&header.h, 0, sizeof(CentralFileHeader));
    writeUInt(header.h.signature, 0x02014b50);

    writeUShort(header.h.version_needed, 0x14);
    writeUInt(header.h.uncompressed_size, contents.length());
    writeMSDosDate(header.h.last_mod_file, QDateTime::currentDateTime());
    QByteArray data = contents;
    if (compression == ZipWriter::AlwaysCompress) {
        writeUShort(header.h.compression_method, 8);

       ulong len = contents.length();
        // shamelessly copied form zlib
        len += (len >> 12) + (len >> 14) + 11;
        int res;
        do {
            data.resize(len);
            res = deflate((uchar*)data.data(), &len, (const uchar*)contents.constData(), contents.length());

            switch (res) {
            case Z_OK:
                data.resize(len);
                break;
            case Z_MEM_ERROR:
                qWarning("QZip: Z_MEM_ERROR: Not enough memory to compress file, skipping");
                data.resize(0);
                break;
            case Z_BUF_ERROR:
                len *= 2;
                break;
            }
        } while (res == Z_BUF_ERROR);
    }
// TODO add a check if data.length() > contents.length().  Then try to store the original and revert the compression method to be uncompressed
    writeUInt(header.h.compressed_size, data.length());
    uint crc_32 = ::crc32(0, 0, 0);
    crc_32 = ::crc32(crc_32, (const uchar *)contents.constData(), contents.length());
    writeUInt(header.h.crc_32, crc_32);

    header.file_name = fileName.toLocal8Bit();
    if (header.file_name.size() > 0xffff) {
        qWarning("QZip: Filename too long, chopping it to 65535 characters");
        header.file_name = header.file_name.left(0xffff);
    }
    writeUShort(header.h.file_name_length, header.file_name.length());
    //h.extra_field_length[2];

    writeUShort(header.h.version_made, 3 << 8);
    //uchar internal_file_attributes[2];
    //uchar external_file_attributes[4];
    quint32 mode = permissionsToMode(permissions);
    switch (type) {
        case File: mode |= S_IFREG; break;
        case Directory: mode |= S_IFDIR; break;
        case Symlink: mode |= S_IFLNK; break;
    }
    writeUInt(header.h.external_file_attributes, mode << 16);
    writeUInt(header.h.offset_local_header, start_of_directory);


    fileHeaders.append(header);

    LocalFileHeader h = header.h.toLocalHeader();
    device->write((const char *)&h, sizeof(LocalFileHeader));
    device->write(header.file_name);
    device->write(data);
    start_of_directory = device->pos();
    dirtyFileTree = true;
}

//////////////////////////////  Reader

/*!
    \class ZipReader::FileInfo
    \internal
    Represents one entry in the zip table of contents.
*/

/*!
    \variable FileInfo::filePath
    The full filepath inside the archive.
*/

/*!
    \variable FileInfo::isDir
    A boolean type indicating if the entry is a directory.
*/

/*!
    \variable FileInfo::isFile
    A boolean type, if it is one this entry is a file.
*/

/*!
    \variable FileInfo::isSymLink
    A boolean type, if it is one this entry is symbolic link.
*/

/*!
    \variable FileInfo::permissions
    A list of flags for the permissions of this entry.
*/

/*!
    \variable FileInfo::crc32
    The calculated checksum as a crc32 type.
*/

/*!
    \variable FileInfo::size
    The total size of the unpacked content.
*/

/*!
    \variable FileInfo::d
    \internal
    private pointer.
*/

/*!
    \class ZipReader
    \internal
    \since 4.5

    \brief the ZipReader class provides a way to inspect the contents of a zip
    archive and extract individual files from it.

    ZipReader can be used to read a zip archive either from a file or from any
    device. An in-memory QBuffer for instance.  The reader can be used to read
    which files are in the archive using fileInfoList() and entryInfoAt() but
    also to extract individual files using fileData() or even to extract all
    files in the archive using extractAll()
*/

/*!
    Create a new zip archive that operates on the \a fileName.  The file will be
    opened with the \a mode.
*/
ZipReader::ZipReader(const QString &archive, QIODevice::OpenMode mode)
{
    std::unique_ptr<QFile> f = std::make_unique<QFile>(archive);
    f->open(mode);
    ZipReader::Status status;
    if (f->error() == QFile::NoError)
        status = NoError;
    else {
        if (f->error() == QFile::ReadError)
            status = FileReadError;
        else if (f->error() == QFile::OpenError)
            status = FileOpenError;
        else if (f->error() == QFile::PermissionsError)
            status = FilePermissionsError;
        else
            status = FileError;
    }

    d = std::make_unique<ZipReaderPrivate>(std::move(f));
    d->status = status;
}

ZipReader::ZipReader(std::unique_ptr<QIODevice> device)
    : d(std::make_unique<ZipReaderPrivate>(std::move(device)))
{
    Q_ASSERT(d->device);
}

/*!
    Desctructor
*/
ZipReader::~ZipReader()
{
    close();
}

/*!
    Returns device used for reading zip archive.
*/
QIODevice* ZipReader::device() const
{
    return d->device;
}

/*!
    Returns true if the user can read the file; otherwise returns false.
*/
bool ZipReader::isReadable() const
{
    return d->device->isReadable();
}

/*!
    Returns true if the file exists; otherwise returns false.
*/
bool ZipReader::exists() const
{
    QFile *f = qobject_cast<QFile*> (d->device);
    if (f == 0)
        return true;
    return f->exists();
}

/*!
    Returns the list of files the archive contains.
*/
QList<ZipReader::FileInfo> ZipReader::fileInfoList() const
{
    d->scanFiles();
    QList<ZipReader::FileInfo> files;
    for (int i = 0; i < d->fileHeaders.size(); ++i) {
        ZipReader::FileInfo fi;
        d->fillFileInfo(i, fi);
        files.append(fi);
    }
    return files;

}

/*!
    Return the number of items in the zip archive.
*/
int ZipReader::count() const
{
    d->scanFiles();
    return d->fileHeaders.count();
}

/*!
    Returns a FileInfo of an entry in the zipfile.
    The \a index is the index into the directory listing of the zipfile.
    Returns an invalid FileInfo if \a index is out of boundaries.

    \sa fileInfoList()
*/
ZipReader::FileInfo ZipReader::entryInfoAt(int index) const
{
    d->scanFiles();
    ZipReader::FileInfo fi;
    if (index >= 0 && index < d->fileHeaders.count())
        d->fillFileInfo(index, fi);
    return fi;
}

/*!
    Fetch the file contents from the zip archive and return the uncompressed bytes.
*/
QByteArray ZipReader::fileData(const QString &fileName) const
{
    QByteArray data;
    if (!fileData(fileName, &data))
        return QByteArray();
    return data;
}

bool ZipReader::fileData(const QString &fileName, QByteArray *data) const
{
    if (!data)
        return false;
    data->clear();

    d->scanFiles();
    if (d->status != ZipReader::NoError)
        return false;

    int index = 0;
    for (; index < d->fileHeaders.size(); ++index) {
        if (QString::fromLocal8Bit(d->fileHeaders.at(index).file_name) == fileName)
            break;
    }
    if (index == d->fileHeaders.size())
        return false;

    const FileHeader header = d->fileHeaders.at(index);
    const uint compressedSize = readUInt(header.h.compressed_size);
    const uint uncompressedSize = readUInt(header.h.uncompressed_size);
    const uint maximumByteArraySize =
        static_cast<uint>(std::numeric_limits<int>::max());
    if (compressedSize > maximumByteArraySize
        || uncompressedSize > maximumByteArraySize) {
        return false;
    }

    const uint start = readUInt(header.h.offset_local_header);
    if (!d->device->seek(start))
        return false;

    LocalFileHeader localHeader;
    if (d->device->read(reinterpret_cast<char *>(&localHeader),
                        sizeof(LocalFileHeader)) != sizeof(LocalFileHeader)
        || readUInt(localHeader.signature) != 0x04034b50) {
        return false;
    }

    const uint skip = readUShort(localHeader.file_name_length)
        + readUShort(localHeader.extra_field_length);
    const qint64 payloadPosition = d->device->pos() + skip;
    if (payloadPosition < d->device->pos()
        || !d->device->seek(payloadPosition)) {
        return false;
    }

    const int compressionMethod = readUShort(localHeader.compression_method);
    if (compressionMethod != readUShort(header.h.compression_method))
        return false;

    const QByteArray compressed = d->device->read(compressedSize);
    if (compressed.size() != static_cast<int>(compressedSize))
        return false;

    QByteArray uncompressed;
    if (compressionMethod == 0) {
        if (compressedSize != uncompressedSize)
            return false;
        uncompressed = compressed;
    } else if (compressionMethod == 8) {
        uncompressed.resize(qMax(static_cast<int>(uncompressedSize), 1));
        ulong length = static_cast<ulong>(uncompressed.size());
        const int result = inflate(
            reinterpret_cast<uchar *>(uncompressed.data()),
            &length,
            reinterpret_cast<const uchar *>(compressed.constData()),
            compressedSize);
        if (result != Z_OK || length != uncompressedSize)
            return false;
        uncompressed.resize(static_cast<int>(uncompressedSize));
    } else {
        return false;
    }

    uLong checksum = crc32(0L, Z_NULL, 0);
    checksum = crc32(
        checksum,
        reinterpret_cast<const Bytef *>(uncompressed.constData()),
        static_cast<uInt>(uncompressed.size()));
    if (static_cast<uint>(checksum) != readUInt(header.h.crc_32))
        return false;

    *data = uncompressed;
    return true;
}

struct ZipExtractionEntry
{
    ZipReader::FileInfo info;
    QString relativePath;
    bool selected;
    QByteArray contents;
};

static bool isWindowsDeviceName(const QString &component)
{
    static const QSet<QString> deviceNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"),
        QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")
    };
    return deviceNames.contains(component.section(QLatin1Char('.'), 0, 0).toUpper());
}

static bool normalizedArchivePath(const ZipReader::FileInfo &fileInfo,
                                  QString *relativePath)
{
    QString path = fileInfo.filePath;
    if (fileInfo.isSymLink || path.isEmpty()
        || path.contains(QChar::Null)
        || path.contains(QLatin1Char('\\'))
        || path.contains(QLatin1Char(':'))
        || QDir::isAbsolutePath(path)
        || path.startsWith(QLatin1Char('/'))) {
        return false;
    }

    if (fileInfo.isDir && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.isEmpty() || path.endsWith(QLatin1Char('/')))
        return false;

    const QStringList components = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    QStringList normalizedComponents;
    for (const QString &component : components) {
        const QString normalizedComponent =
            component.normalized(QString::NormalizationForm_C);
        if (normalizedComponent.isEmpty()
            || normalizedComponent == QLatin1String(".")
            || normalizedComponent == QLatin1String("..")
            || normalizedComponent.endsWith(QLatin1Char('.'))
            || normalizedComponent.endsWith(QLatin1Char(' '))
            || isWindowsDeviceName(normalizedComponent)) {
            return false;
        }
        normalizedComponents.append(normalizedComponent);
    }

    const QString normalized = normalizedComponents.join(QLatin1Char('/'));
    if (QDir::cleanPath(normalized) != normalized)
        return false;

    *relativePath = normalized;
    return true;
}

static bool fileSystemPathIsLink(const QFileInfo &fileInfo)
{
#if defined(Q_OS_WIN)
    const QString nativePath =
        QDir::toNativeSeparators(fileInfo.absoluteFilePath());
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
#endif
    return fileInfo.isSymLink();
}

static bool destinationPathIsSafe(const QDir &baseDir,
                                  const QString &relativePath,
                                  bool directory)
{
    QString currentPath = baseDir.absolutePath();
    const QStringList components =
        relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    for (int index = 0; index < components.size(); ++index) {
        currentPath = QDir(currentPath).filePath(components.at(index));
        const QFileInfo currentInfo(currentPath);
        if (fileSystemPathIsLink(currentInfo))
            return false;
        if (!currentInfo.exists())
            continue;

        const bool mustBeDirectory =
            directory || index < components.size() - 1;
        if (!mustBeDirectory)
            return false;
        if (!currentInfo.isDir())
            return false;
    }
    return true;
}

static bool pathContainsSymlink(QString absolutePath)
{
    absolutePath = QDir::cleanPath(absolutePath);
    while (true) {
        const QFileInfo pathInfo(absolutePath);
        if (fileSystemPathIsLink(pathInfo))
            return true;

        const QString parentPath = pathInfo.dir().absolutePath();
        if (parentPath == absolutePath)
            return false;
        absolutePath = parentPath;
    }
}

static bool createDirectoryPath(const QDir &baseDir,
                                const QString &relativePath,
                                QStringList *createdDirectories)
{
    if (relativePath.isEmpty() || relativePath == QLatin1String("."))
        return true;

    QString currentAbsolutePath = baseDir.absolutePath();
    QString currentRelativePath;
    const QStringList components =
        relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        const QString parentAbsolutePath = currentAbsolutePath;
        currentAbsolutePath =
            QDir(parentAbsolutePath).filePath(component);
        if (!currentRelativePath.isEmpty())
            currentRelativePath.append(QLatin1Char('/'));
        currentRelativePath.append(component);

        const QFileInfo existingInfo(currentAbsolutePath);
        if (fileSystemPathIsLink(existingInfo))
            return false;
        if (existingInfo.exists()) {
            if (!existingInfo.isDir())
                return false;
            continue;
        }

        if (!QDir(parentAbsolutePath).mkdir(component))
            return false;
        createdDirectories->append(currentRelativePath);

        const QFileInfo createdInfo(currentAbsolutePath);
        if (fileSystemPathIsLink(createdInfo) || !createdInfo.isDir())
            return false;
    }
    return true;
}

/*!
    Extracts the selected contents of the zip file into \a destinationDir.
    All member names are validated before the filesystem is changed. If
    \a allowedRelativeFiles is null, every regular file is selected.
*/
bool ZipReader::extractAll(const QString &destinationDir,
                           QStringList *extractedRelativeFiles,
                           const QList<QString> *allowedRelativeFiles) const
{
    if (extractedRelativeFiles)
        extractedRelativeFiles->clear();

    QSet<QString> allowedPaths;
    if (allowedRelativeFiles) {
        for (const QString &requestedPath : *allowedRelativeFiles) {
            FileInfo requestedInfo;
            requestedInfo.filePath = requestedPath;
            QString normalizedPath;
            if (!normalizedArchivePath(requestedInfo, &normalizedPath))
                return false;
            allowedPaths.insert(normalizedPath);
        }
    }

    const QString rootPath = QDir(destinationDir).absolutePath();
    const QFileInfo rootInfo(rootPath);
    if (pathContainsSymlink(rootPath)
        || (rootInfo.exists() && !rootInfo.isDir()))
        return false;

    const QDir baseDir(rootPath);
    const QList<FileInfo> allFiles = fileInfoList();
    if (status() != NoError)
        return false;

    QList<ZipExtractionEntry> entries;
    QHash<QString, bool> entryTypes;

    // Validate the complete archive before creating any filesystem objects.
    for (const FileInfo &fileInfo : allFiles) {
        QString relativePath;
        if (!normalizedArchivePath(fileInfo, &relativePath))
            return false;

        const QString key = relativePath.toCaseFolded();
        if (entryTypes.contains(key))
            return false;

        entryTypes.insert(key, fileInfo.isDir);
        const bool selected = !allowedRelativeFiles
            || (!fileInfo.isDir && allowedPaths.contains(relativePath));
        entries.append({ fileInfo, relativePath, selected, QByteArray() });

        if (!destinationPathIsSafe(baseDir, relativePath, fileInfo.isDir))
            return false;
    }

    // A file entry may not be used as a parent by another archive member.
    for (const ZipExtractionEntry &entry : entries) {
        const QStringList components =
            entry.relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString parentPath;
        for (int index = 0; index < components.size() - 1; ++index) {
            if (!parentPath.isEmpty())
                parentPath.append(QLatin1Char('/'));
            parentPath.append(components.at(index));

            const auto parent = entryTypes.constFind(parentPath.toCaseFolded());
            if (parent != entryTypes.constEnd() && !parent.value())
                return false;
        }
    }

    // Validate and decompress selected members before writing anything.
    for (ZipExtractionEntry &entry : entries) {
        if (!entry.selected || entry.info.isDir)
            continue;
        if (!fileData(entry.info.filePath, &entry.contents)
            || entry.contents.size() != entry.info.size) {
            return false;
        }
    }

    const bool rootExisted = rootInfo.exists();
    if (!QDir().mkpath(rootPath))
        return false;
    if (pathContainsSymlink(rootPath)) {
        if (!rootExisted)
            QDir().rmdir(rootPath);
        return false;
    }

    QStringList extracted;
    QStringList createdDirectories;
    const auto rollback = [&]() {
        const QFile::Permissions ownerDirectoryPermissions =
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        for (const QString &relativePath : createdDirectories) {
            QFile::setPermissions(
                baseDir.absoluteFilePath(relativePath),
                ownerDirectoryPermissions);
        }
        for (const QString &relativePath : extracted) {
            QFile output(baseDir.absoluteFilePath(relativePath));
            output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            output.remove();
        }
        for (int index = createdDirectories.size() - 1; index >= 0; --index)
            QDir().rmdir(baseDir.absoluteFilePath(createdDirectories.at(index)));
        if (!rootExisted)
            QDir().rmdir(rootPath);
        extracted.clear();
        return false;
    };

    // Create explicit directory entries before regular files.
    for (const ZipExtractionEntry &entry : entries) {
        if (!entry.selected || !entry.info.isDir)
            continue;
        if (!destinationPathIsSafe(baseDir, entry.relativePath, true)
            || !createDirectoryPath(
                baseDir, entry.relativePath, &createdDirectories)) {
            return rollback();
        }
    }
    for (const ZipExtractionEntry &entry : entries) {
        if (!entry.selected || entry.info.isDir)
            continue;

        const QString parentPath = QFileInfo(entry.relativePath).path();
        if (!createDirectoryPath(
                baseDir, parentPath, &createdDirectories)) {
            return rollback();
        }
        if (!destinationPathIsSafe(baseDir, entry.relativePath, false))
            return rollback();

        QSaveFile output(baseDir.absoluteFilePath(entry.relativePath));
        if (!output.open(QIODevice::WriteOnly)
            || output.write(entry.contents) != entry.contents.size()) {
            output.cancelWriting();
            return rollback();
        }
        if (entry.info.permissions > 0
            && !output.setPermissions(entry.info.permissions)) {
            output.cancelWriting();
            return rollback();
        }
        if (!output.commit())
            return rollback();

        extracted.append(entry.relativePath);
    }

    QList<const ZipExtractionEntry *> directoriesToFinalize;
    for (const ZipExtractionEntry &entry : entries) {
        if (entry.selected && entry.info.isDir
            && entry.info.permissions > 0
            && createdDirectories.contains(entry.relativePath)) {
            directoriesToFinalize.append(&entry);
        }
    }
    std::sort(
        directoriesToFinalize.begin(),
        directoriesToFinalize.end(),
        [](const ZipExtractionEntry *left, const ZipExtractionEntry *right) {
            return left->relativePath.count(QLatin1Char('/'))
                > right->relativePath.count(QLatin1Char('/'));
        });
    for (const ZipExtractionEntry *entry : directoriesToFinalize) {
        if (!QFile::setPermissions(
                baseDir.absoluteFilePath(entry->relativePath),
                entry->info.permissions)) {
            return rollback();
        }
    }

    if (extractedRelativeFiles)
        *extractedRelativeFiles = extracted;
    return true;
}

/*!
    \enum ZipReader::Status

    The following status values are possible:

    \value NoError  No error occurred.
    \value FileReadError    An error occurred when reading from the file.
    \value FileOpenError    The file could not be opened.
    \value FilePermissionsError The file could not be accessed.
    \value FileError        Another file error occurred.
*/

/*!
    Returns a status code indicating the first error that was met by ZipReader,
    or ZipReader::NoError if no error occurred.
*/
ZipReader::Status ZipReader::status() const
{
    return d->status;
}

/*!
    Close the zip file.
*/
void ZipReader::close()
{
    d->device->close();
}

////////////////////////////// Writer

/*!
    \class ZipWriter
    \internal
    \since 4.5

    \brief the ZipWriter class provides a way to create a new zip archive.

    ZipWriter can be used to create a zip archive containing any number of files
    and directories. The files in the archive will be compressed in a way that is
    compatible with common zip reader applications.
*/


/*!
    Create a new zip archive that operates on the \a archive filename.  The file will
    be opened with the \a mode.
    \sa isValid()
*/
ZipWriter::ZipWriter(const QString &fileName, QIODevice::OpenMode mode)
{
    std::unique_ptr<QFile> f = std::make_unique<QFile>(fileName);
    f->open(mode);
    ZipWriter::Status status;
    if (f->error() == QFile::NoError)
        status = ZipWriter::NoError;
    else {
        if (f->error() == QFile::WriteError)
            status = ZipWriter::FileWriteError;
        else if (f->error() == QFile::OpenError)
            status = ZipWriter::FileOpenError;
        else if (f->error() == QFile::PermissionsError)
            status = ZipWriter::FilePermissionsError;
        else
            status = ZipWriter::FileError;
    }

    d = std::make_unique<ZipWriterPrivate>(std::move(f));
    d->status = status;
}

ZipWriter::~ZipWriter()
{
    close();
}

/*!
    Returns device used for writing zip archive.
*/
QIODevice* ZipWriter::device() const
{
    return d->device;
}

/*!
    Returns true if the user can write to the archive; otherwise returns false.
*/
bool ZipWriter::isWritable() const
{
    return d->device->isWritable();
}

/*!
    Returns true if the file exists; otherwise returns false.
*/
bool ZipWriter::exists() const
{
    QFile *f = qobject_cast<QFile*> (d->device);
    if (f == 0)
        return true;
    return f->exists();
}

/*!
    \enum ZipWriter::Status

    The following status values are possible:

    \value NoError  No error occurred.
    \value FileWriteError    An error occurred when writing to the device.
    \value FileOpenError    The file could not be opened.
    \value FilePermissionsError The file could not be accessed.
    \value FileError        Another file error occurred.
*/

/*!
    Returns a status code indicating the first error that was met by ZipWriter,
    or ZipWriter::NoError if no error occurred.
*/
ZipWriter::Status ZipWriter::status() const
{
    return d->status;
}

/*!
    \enum ZipWriter::CompressionPolicy

    \value AlwaysCompress   A file that is added is compressed.
    \value NeverCompress    A file that is added will be stored without changes.
    \value AutoCompress     A file that is added will be compressed only if that will give a smaller file.
*/

/*!
     Sets the policy for compressing newly added files to the new \a policy.

    \note the default policy is AlwaysCompress

    \sa compressionPolicy()
    \sa addFile()
*/
void ZipWriter::setCompressionPolicy(CompressionPolicy policy)
{
    d->compressionPolicy = policy;
}

/*!
     Returns the currently set compression policy.
    \sa setCompressionPolicy()
    \sa addFile()
*/
ZipWriter::CompressionPolicy ZipWriter::compressionPolicy() const
{
    return d->compressionPolicy;
}

/*!
    Sets the permissions that will be used for newly added files.

    \note the default permissions are QFile::ReadOwner | QFile::WriteOwner.

    \sa creationPermissions()
    \sa addFile()
*/
void ZipWriter::setCreationPermissions(QFile::Permissions permissions)
{
    d->permissions = permissions;
}

/*!
     Returns the currently set creation permissions.

    \sa setCreationPermissions()
    \sa addFile()
*/
QFile::Permissions ZipWriter::creationPermissions() const
{
    return d->permissions;
}

/*!
    Add a file to the archive with \a data as the file contents.
    The file will be stored in the archive using the \a fileName which
    includes the full path in the archive.

    The new file will get the file permissions based on the current
    creationPermissions and it will be compressed using the zip compression
    based on the current compression policy.

    \sa setCreationPermissions()
    \sa setCompressionPolicy()
*/
void ZipWriter::addFile(const QString &fileName, const QByteArray &data)
{
    d->addEntry(ZipWriterPrivate::File, QDir::fromNativeSeparators(fileName), data);
}

/*!
    Add a file to the archive with \a device as the source of the contents.
    The contents returned from QIODevice::readAll() will be used as the
    filedata.
    The file will be stored in the archive using the \a fileName which
    includes the full path in the archive.
*/
void ZipWriter::addFile(const QString &fileName, QIODevice *device)
{
    Q_ASSERT(device);
    QIODevice::OpenMode mode = device->openMode();
    bool opened = false;
    if ((mode & QIODevice::ReadOnly) == 0) {
        opened = true;
        if (! device->open(QIODevice::ReadOnly)) {
            d->status = FileOpenError;
            return;
        }
    }
    d->addEntry(ZipWriterPrivate::File, QDir::fromNativeSeparators(fileName), device->readAll());
    if (opened)
        device->close();
}

/*!
    Create a new directory in the archive with the specified \a dirName and
    the \a permissions;
*/
void ZipWriter::addDirectory(const QString &dirName)
{
    QString name(QDir::fromNativeSeparators(dirName));
    // separator is mandatory
    if (!name.endsWith(QLatin1Char('/')))
        name.append(QLatin1Char('/'));
    d->addEntry(ZipWriterPrivate::Directory, name, QByteArray());
}

/*!
    Create a new symbolic link in the archive with the specified \a dirName
    and the \a permissions;
    A symbolic link contains the destination (relative) path and name.
*/
void ZipWriter::addSymLink(const QString &fileName, const QString &destination)
{
    d->addEntry(ZipWriterPrivate::Symlink, QDir::fromNativeSeparators(fileName), QFile::encodeName(destination));
}

/*!
   Closes the zip file.
*/
void ZipWriter::close()
{
    if (!(d->device->openMode() & QIODevice::WriteOnly)) {
        d->device->close();
        return;
    }

    //qDebug("QZip::close writing directory, %d entries", d->fileHeaders.size());
    d->device->seek(d->start_of_directory);
    // write new directory
    for (int i = 0; i < d->fileHeaders.size(); ++i) {
        const FileHeader &header = d->fileHeaders.at(i);
        d->device->write((const char *)&header.h, sizeof(CentralFileHeader));
        d->device->write(header.file_name);
        d->device->write(header.extra_field);
        d->device->write(header.file_comment);
    }
    int dir_size = d->device->pos() - d->start_of_directory;
    // write end of directory
    EndOfDirectory eod;
    memset(&eod, 0, sizeof(EndOfDirectory));
    writeUInt(eod.signature, 0x06054b50);
    //uchar this_disk[2];
    //uchar start_of_directory_disk[2];
    writeUShort(eod.num_dir_entries_this_disk, d->fileHeaders.size());
    writeUShort(eod.num_dir_entries, d->fileHeaders.size());
    writeUInt(eod.directory_size, dir_size);
    writeUInt(eod.dir_start_offset, d->start_of_directory);
    writeUShort(eod.comment_length, d->comment.length());

    d->device->write((const char *)&eod, sizeof(EndOfDirectory));
    d->device->write(d->comment);
    d->device->close();
}

QT_END_NAMESPACE

#endif // QT_NO_TEXTODFWRITER
