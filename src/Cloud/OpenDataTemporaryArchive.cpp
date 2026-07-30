/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif

#include "OpenDataTemporaryArchive.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QCoreApplication>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <cerrno>
#include <cstdint>
#include <utility>

namespace OpenDataTemporaryArchive {

class WorkspaceGuard final
{
public:
#ifdef Q_OS_UNIX
    explicit WorkspaceGuard(int descriptor)
        : descriptor_(descriptor)
    {
    }

    ~WorkspaceGuard()
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }

    int descriptor() const
    {
        return descriptor_;
    }

    bool isValid() const
    {
        return descriptor_ >= 0;
    }

private:
    int descriptor_ = -1;
#elif defined(Q_OS_WIN)
    explicit WorkspaceGuard(HANDLE handle)
        : handle_(handle)
    {
    }

    ~WorkspaceGuard()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }

    HANDLE handle() const
    {
        return handle_;
    }

    bool isValid() const
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    WorkspaceGuard() = default;
    ~WorkspaceGuard() = default;
#endif
};

} // namespace OpenDataTemporaryArchive

namespace {

constexpr int WorkspaceLockStaleTimeMs =
    24 * 60 * 60 * 1000;

bool isReparsePoint(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes =
        GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    Q_UNUSED(path)
    return false;
#endif
}

QString canonicalDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isAbsolute()
        || !info.exists()
        || !info.isDir()
        || info.isSymLink()
        || isReparsePoint(path)) {
        return {};
    }
#ifdef Q_OS_UNIX
    const QFileDevice::Permissions unsafeWrites =
        QFileDevice::WriteGroup
        | QFileDevice::WriteOther;
    if (info.ownerId() != uint(::geteuid())
        || (info.permissions() & unsafeWrites)
            != QFileDevice::Permissions()) {
        return {};
    }
#endif
    return QDir::fromNativeSeparators(
        QDir(path).canonicalPath());
}

bool isOwnedWorkspace(
    const QString &root,
    const QString &path)
{
    const QFileInfo info(path);
    return !root.isEmpty()
        && info.isAbsolute()
        && info.exists()
        && info.isDir()
        && !info.isSymLink()
        && !isReparsePoint(path)
        && QDir::fromNativeSeparators(
               info.dir().canonicalPath()) == root
        && info.fileName().startsWith(
               QStringLiteral("gc-opendata-"));
}

QString lockPath(const QString &workspacePath)
{
    return workspacePath + QStringLiteral(".lock");
}

#ifdef Q_OS_WIN
class WindowsHandle final
{
public:
    explicit WindowsHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~WindowsHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }

    WindowsHandle(const WindowsHandle &) = delete;
    WindowsHandle &operator=(const WindowsHandle &) = delete;

    HANDLE get() const
    {
        return handle_;
    }

    HANDLE release()
    {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE)
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool currentWindowsUserSid(
    QByteArray &storage,
    PSID &sid)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &rawToken)) {
        return false;
    }
    WindowsHandle token(rawToken);

    DWORD required = 0;
    GetTokenInformation(
        token.get(),
        TokenUser,
        nullptr,
        0,
        &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        return false;
    }
    storage.resize(int(required));
    if (!GetTokenInformation(
            token.get(),
            TokenUser,
            storage.data(),
            required,
            &required)) {
        return false;
    }
    auto *user =
        reinterpret_cast<TOKEN_USER *>(storage.data());
    if (!IsValidSid(user->User.Sid))
        return false;
    sid = user->User.Sid;
    return true;
}

class WindowsPrivateSecurity final
{
public:
    explicit WindowsPrivateSecurity(bool directory)
    {
        PSID userSid = nullptr;
        if (!currentWindowsUserSid(
                userStorage_, userSid)) {
            return;
        }
        const DWORD aclSize = DWORD(
            sizeof(ACL)
            + sizeof(ACCESS_ALLOWED_ACE)
            - sizeof(DWORD)
            + GetLengthSid(userSid));
        aclStorage_.resize(int(aclSize));
        auto *acl =
            reinterpret_cast<PACL>(aclStorage_.data());
        const BYTE inheritanceFlags =
            directory
                ? CONTAINER_INHERIT_ACE
                    | OBJECT_INHERIT_ACE
                : 0;
        if (!InitializeAcl(
                acl, aclSize, ACL_REVISION)
            || !AddAccessAllowedAceEx(
                acl,
                ACL_REVISION,
                inheritanceFlags,
                FILE_ALL_ACCESS,
                userSid)
            || !InitializeSecurityDescriptor(
                &descriptor_,
                SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorOwner(
                &descriptor_, userSid, FALSE)
            || !SetSecurityDescriptorDacl(
                &descriptor_, TRUE, acl, FALSE)
            || !SetSecurityDescriptorControl(
                &descriptor_,
                SE_DACL_PROTECTED,
                SE_DACL_PROTECTED)) {
            return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
        valid_ = true;
    }

    SECURITY_ATTRIBUTES *attributes()
    {
        return valid_ ? &attributes_ : nullptr;
    }

private:
    QByteArray userStorage_;
    QByteArray aclStorage_;
    SECURITY_DESCRIPTOR descriptor_ {};
    SECURITY_ATTRIBUTES attributes_ {};
    bool valid_ = false;
};

QString windowsHandlePath(HANDLE handle)
{
    const DWORD flags =
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required =
        GetFinalPathNameByHandleW(
            handle, nullptr, 0, flags);
    if (required == 0) return {};
    QString path;
    path.resize(int(required));
    const DWORD written =
        GetFinalPathNameByHandleW(
            handle,
            reinterpret_cast<LPWSTR>(path.data()),
            required,
            flags);
    if (written == 0 || written >= required)
        return {};
    path.resize(int(written));
    if (path.startsWith(
            QStringLiteral("\\\\?\\UNC\\"))) {
        path = QStringLiteral("//") + path.mid(8);
    } else if (path.startsWith(
                   QStringLiteral("\\\\?\\"))) {
        path.remove(0, 4);
    }
    return QDir::fromNativeSeparators(
        QDir::cleanPath(path));
}

bool windowsHandleSupportsPersistentAcls(HANDLE handle)
{
    DWORD fileSystemFlags = 0;
    return handle != INVALID_HANDLE_VALUE
        && GetVolumeInformationByHandleW(
            handle,
            nullptr, 0,
            nullptr, nullptr,
            &fileSystemFlags,
            nullptr, 0)
        && (fileSystemFlags & FILE_PERSISTENT_ACLS);
}

bool windowsHandleHasPrivateSecurity(
    HANDLE handle,
    bool directory)
{
    BY_HANDLE_FILE_INFORMATION information {};
    if (handle == INVALID_HANDLE_VALUE
        || !GetFileInformationByHandle(
            handle, &information)
        || bool(information.dwFileAttributes
                & FILE_ATTRIBUTE_DIRECTORY)
            != directory
        || information.dwFileAttributes
            & FILE_ATTRIBUTE_REPARSE_POINT
        || (!directory
            && information.nNumberOfLinks != 1)
        || !windowsHandleSupportsPersistentAcls(
            handle)) {
        return false;
    }

    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsUserSid(
            userStorage, userSid)) {
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION
            | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &acl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS)
        return false;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool privateSecurity =
        owner
        && EqualSid(owner, userSid)
        && acl
        && acl->AceCount == 1
        && GetSecurityDescriptorControl(
            descriptor, &control, &revision)
        && (control & SE_DACL_PROTECTED);
    if (privateSecurity) {
        void *rawAce = nullptr;
        if (!GetAce(acl, 0, &rawAce)) {
            privateSecurity = false;
        } else {
            auto *header =
                static_cast<ACE_HEADER *>(rawAce);
            auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(
                    rawAce);
            const BYTE inheritanceFlags =
                OBJECT_INHERIT_ACE
                | CONTAINER_INHERIT_ACE
                | INHERITED_ACE
                | INHERIT_ONLY_ACE
                | NO_PROPAGATE_INHERIT_ACE;
            const BYTE expectedInheritance =
                directory
                    ? OBJECT_INHERIT_ACE
                        | CONTAINER_INHERIT_ACE
                    : 0;
            privateSecurity =
                header->AceType
                    == ACCESS_ALLOWED_ACE_TYPE
                && (header->AceFlags
                    & inheritanceFlags)
                    == expectedInheritance
                && IsValidSid(&ace->SidStart)
                && EqualSid(
                    &ace->SidStart, userSid)
                && (ace->Mask & FILE_ALL_ACCESS)
                    == FILE_ALL_ACCESS;
        }
    }
    if (descriptor)
        LocalFree(descriptor);
    return privateSecurity;
}

bool setWindowsHandleDeletion(
    HANDLE handle,
    bool enabled)
{
    FILE_DISPOSITION_INFO disposition {
        enabled ? TRUE : FALSE
    };
    return SetFileInformationByHandle(
        handle,
        FileDispositionInfo,
        &disposition,
        sizeof(disposition));
}
#endif

bool createPrivateWorkspace(
    const QString &root,
    const QString &workspaceName,
    std::unique_ptr<
        OpenDataTemporaryArchive::WorkspaceGuard>
        &workspaceGuard,
    QString &error,
    bool forcePostCreationValidationFailure = false)
{
    error.clear();
    workspaceGuard.reset();
    const QString path =
        QDir(root).filePath(workspaceName);
#ifdef Q_OS_UNIX
    Q_UNUSED(forcePostCreationValidationFailure)
    if (::mkdir(
            QFile::encodeName(path).constData(),
            S_IRWXU) != 0) {
        error = QStringLiteral(
            "Cannot create the OpenData temporary workspace");
        return false;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor =
        ::open(
            QFile::encodeName(path).constData(),
            flags);
    struct stat status {};
    if (descriptor < 0
        || ::fstat(descriptor, &status) != 0
        || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid()) {
        if (descriptor >= 0)
            ::close(descriptor);
        QDir(root).rmdir(workspaceName);
        error = QStringLiteral(
            "Cannot secure the OpenData temporary workspace");
        return false;
    }
    workspaceGuard = std::make_unique<
        OpenDataTemporaryArchive::WorkspaceGuard>(
            descriptor);
    return true;
#elif defined(Q_OS_WIN)
    WindowsPrivateSecurity security(true);
    if (!security.attributes()) {
        error = QStringLiteral(
            "Cannot create a private OpenData temporary workspace");
        return false;
    }
    if (!CreateDirectoryW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            security.attributes())) {
        error = QStringLiteral(
            "Cannot create a private OpenData temporary workspace");
        return false;
    }

    WindowsHandle handle(
        CreateFileW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            FILE_LIST_DIRECTORY
                | FILE_READ_ATTRIBUTES
                | READ_CONTROL
                | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    const QString expectedPath =
        QDir::fromNativeSeparators(
            QDir::cleanPath(path));
    const QString openedPath =
        handle.get() == INVALID_HANDLE_VALUE
            ? QString()
            : windowsHandlePath(handle.get());
    const bool exactPath =
        openedPath == expectedPath;
    const bool validSecurity =
        handle.get() != INVALID_HANDLE_VALUE
        && windowsHandleHasPrivateSecurity(
            handle.get(), true);
    if (handle.get() == INVALID_HANDLE_VALUE
        || !exactPath
        || !validSecurity
        || forcePostCreationValidationFailure) {
        if (handle.get() != INVALID_HANDLE_VALUE
            && exactPath) {
            setWindowsHandleDeletion(
                handle.get(), true);
        }
        handle.reset();
        error = QStringLiteral(
            "Cannot create a private OpenData temporary workspace");
        return false;
    }
    workspaceGuard = std::make_unique<
        OpenDataTemporaryArchive::WorkspaceGuard>(
            handle.release());
    return true;
#else
    Q_UNUSED(forcePostCreationValidationFailure)
    if (!QDir(root).mkdir(workspaceName)) {
        error = QStringLiteral(
            "Cannot create a private OpenData temporary workspace");
        return false;
    }
    if (!QFile::setPermissions(
            path,
            QFileDevice::ReadOwner
                | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        QDir(root).rmdir(workspaceName);
        error = QStringLiteral(
            "Cannot create a private OpenData temporary workspace");
        return false;
    }
    workspaceGuard = std::make_unique<
        OpenDataTemporaryArchive::WorkspaceGuard>();
    return true;
#endif
}

#ifdef Q_OS_UNIX
class FileDescriptor final
{
public:
    explicit FileDescriptor(int descriptor = -1)
        : descriptor_(descriptor)
    {
    }

    ~FileDescriptor()
    {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    int get() const
    {
        return descriptor_;
    }

    int release()
    {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

private:
    int descriptor_ = -1;
};

int directoryOpenFlags()
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

std::unique_ptr<OpenDataTemporaryArchive::TemporaryFile>
createWorkspaceFile(
    const QString &root,
    const QString &workspacePath,
    OpenDataTemporaryArchive::WorkspaceGuard *workspaceGuard,
    const QString &fileTemplate,
    QString &error)
{
    error.clear();
    const QString workspaceName =
        QFileInfo(workspacePath).fileName();
    if (!workspaceGuard
        || !workspaceGuard->isValid()
        || !isOwnedWorkspace(root, workspacePath)) {
        error = QStringLiteral(
            "Invalid OpenData temporary workspace");
        return {};
    }

    FileDescriptor rootDescriptor(
        ::open(
            QFile::encodeName(root).constData(),
            directoryOpenFlags()));
    const QByteArray encodedWorkspace =
        QFile::encodeName(workspaceName);
    FileDescriptor workspaceDescriptor(
        ::dup(workspaceGuard->descriptor()));
    struct stat guardedStatus {};
    struct stat namedStatus {};
    if (rootDescriptor.get() < 0
        || workspaceDescriptor.get() < 0
        || ::fstat(
               workspaceDescriptor.get(),
               &guardedStatus) != 0
        || ::fstatat(
               rootDescriptor.get(),
               encodedWorkspace.constData(),
               &namedStatus,
               AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISDIR(guardedStatus.st_mode)
        || !S_ISDIR(namedStatus.st_mode)
        || guardedStatus.st_dev != namedStatus.st_dev
        || guardedStatus.st_ino != namedStatus.st_ino) {
        error = QStringLiteral(
            "Cannot securely open the OpenData temporary workspace");
        return {};
    }

    int creationFlags =
        O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    creationFlags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    creationFlags |= O_NOFOLLOW;
#endif

    for (int attempt = 0; attempt < 10; ++attempt) {
        QString leafName = fileTemplate;
        leafName.replace(
            QStringLiteral("XXXXXX"),
            QUuid::createUuid()
                .toString(QUuid::WithoutBraces)
                .remove(QLatin1Char('-'))
                .left(12));
        const QByteArray encodedLeaf =
            QFile::encodeName(leafName);
        FileDescriptor fileDescriptor(
            ::openat(
                workspaceDescriptor.get(),
                encodedLeaf.constData(),
                creationFlags,
                S_IRUSR | S_IWUSR));
        if (fileDescriptor.get() < 0) {
            if (errno == EEXIST) continue;
            error = QStringLiteral(
                "Cannot create a private OpenData temporary file");
            return {};
        }

        struct stat status {};
        if (::fstat(fileDescriptor.get(), &status) != 0
            || !S_ISREG(status.st_mode)) {
            ::unlinkat(
                workspaceDescriptor.get(),
                encodedLeaf.constData(),
                0);
            error = QStringLiteral(
                "Cannot create a private OpenData temporary file");
            return {};
        }

        const QString path =
            QDir(workspacePath).filePath(leafName);
        auto file = std::make_unique<QFile>(path);
        const int descriptor = fileDescriptor.release();
        if (!file->open(
                descriptor,
                QIODevice::ReadWrite,
                QFileDevice::AutoCloseHandle)) {
            ::close(descriptor);
            ::unlinkat(
                workspaceDescriptor.get(),
                encodedLeaf.constData(),
                0);
            error = QStringLiteral(
                "Cannot access a private OpenData temporary file");
            return {};
        }
        return std::make_unique<
            OpenDataTemporaryArchive::TemporaryFile>(
                path, std::move(file));
    }

    error = QStringLiteral(
        "Cannot allocate a unique OpenData temporary file");
    return {};
}

bool removeWorkspace(
    const QString &root,
    const QString &path,
    OpenDataTemporaryArchive::WorkspaceGuard *workspaceGuard,
    QString &error)
{
    error.clear();
    const QFileInfo workspaceInfo(path);
    const QString workspaceName = workspaceInfo.fileName();
    const QString workspaceParent =
        QDir::fromNativeSeparators(
            QDir::cleanPath(
                workspaceInfo.dir().absolutePath()));
    if (root.isEmpty()
        || !workspaceInfo.isAbsolute()
        || workspaceParent != root
        || workspaceName.isEmpty()
        || !workspaceName.startsWith(
            QStringLiteral("gc-opendata-"))
        || workspaceName.contains(QLatin1Char('/'))
        || workspaceName.contains(QLatin1Char('\\'))
        || ((!workspaceGuard
             || !workspaceGuard->isValid())
            && !isOwnedWorkspace(root, path))) {
        error = QStringLiteral(
            "Invalid OpenData temporary workspace");
        return false;
    }

    FileDescriptor rootDescriptor(
        ::open(
            QFile::encodeName(root).constData(),
            directoryOpenFlags()));
    if (rootDescriptor.get() < 0) {
        error = QStringLiteral(
            "Cannot open the OpenData temporary directory");
        return false;
    }
    const QByteArray encodedName =
        QFile::encodeName(workspaceName);
    FileDescriptor workspaceDescriptor(
        workspaceGuard && workspaceGuard->isValid()
            ? ::dup(workspaceGuard->descriptor())
            : ::openat(
                  rootDescriptor.get(),
                  encodedName.constData(),
                  directoryOpenFlags()));
    struct stat openedWorkspace {};
    if (workspaceDescriptor.get() < 0
        || ::fstat(
               workspaceDescriptor.get(),
               &openedWorkspace) != 0
        || !S_ISDIR(openedWorkspace.st_mode)
        || openedWorkspace.st_uid != ::geteuid()) {
        error = QStringLiteral(
            "Cannot securely open the OpenData temporary workspace");
        return false;
    }

    const int enumerationDescriptor =
        ::lseek(
            workspaceDescriptor.get(),
            0,
            SEEK_SET) < 0
            ? -1
            : ::dup(workspaceDescriptor.get());
    if (enumerationDescriptor < 0) {
        error = QStringLiteral(
            "Cannot inspect the OpenData temporary workspace");
        return false;
    }
    DIR *entries = ::fdopendir(enumerationDescriptor);
    if (!entries) {
        ::close(enumerationDescriptor);
        error = QStringLiteral(
            "Cannot inspect the OpenData temporary workspace");
        return false;
    }

    bool success = true;
    errno = 0;
    while (dirent *entry = ::readdir(entries)) {
        const QByteArray name(entry->d_name);
        if (name == QByteArrayLiteral(".")
            || name == QByteArrayLiteral("..")) {
            errno = 0;
            continue;
        }

        struct stat status {};
        if (::fstatat(
                workspaceDescriptor.get(),
                name.constData(),
                &status,
                AT_SYMLINK_NOFOLLOW) != 0
            || S_ISDIR(status.st_mode)
            || (!S_ISREG(status.st_mode)
                && !S_ISLNK(status.st_mode))
            || ::unlinkat(
                workspaceDescriptor.get(),
                name.constData(),
                0) != 0) {
            success = false;
            break;
        }
        errno = 0;
    }
    if (errno != 0) success = false;
    ::closedir(entries);

    QByteArray removalName;
    const int rootEnumerationDescriptor =
        ::dup(rootDescriptor.get());
    DIR *rootEntries =
        rootEnumerationDescriptor >= 0
            ? ::fdopendir(rootEnumerationDescriptor)
            : nullptr;
    if (!rootEntries) {
        if (rootEnumerationDescriptor >= 0)
            ::close(rootEnumerationDescriptor);
        success = false;
    } else {
        errno = 0;
        while (dirent *entry = ::readdir(rootEntries)) {
            const QByteArray name(entry->d_name);
            if (name == QByteArrayLiteral(".")
                || name == QByteArrayLiteral("..")) {
                errno = 0;
                continue;
            }
            struct stat candidate {};
            if (::fstatat(
                    rootDescriptor.get(),
                    name.constData(),
                    &candidate,
                    AT_SYMLINK_NOFOLLOW) == 0
                && S_ISDIR(candidate.st_mode)
                && candidate.st_dev
                    == openedWorkspace.st_dev
                && candidate.st_ino
                    == openedWorkspace.st_ino) {
                if (!removalName.isEmpty()) {
                    success = false;
                    break;
                }
                removalName = name;
            }
            errno = 0;
        }
        if (errno != 0)
            success = false;
        ::closedir(rootEntries);
    }
    if (success
        && removalName.isEmpty()
        && openedWorkspace.st_nlink == 0) {
        return true;
    }
    if (!success
        || removalName.isEmpty()
        || ::unlinkat(
            rootDescriptor.get(),
            removalName.constData(),
            AT_REMOVEDIR) != 0) {
        error = QStringLiteral(
            "Cannot remove OpenData temporary workspace");
        return false;
    }
    return true;
}
#elif defined(Q_OS_WIN)
std::unique_ptr<OpenDataTemporaryArchive::TemporaryFile>
createWorkspaceFile(
    const QString &root,
    const QString &workspacePath,
    OpenDataTemporaryArchive::WorkspaceGuard *workspaceGuard,
    const QString &fileTemplate,
    QString &error)
{
    error.clear();
    if (!workspaceGuard
        || !workspaceGuard->isValid()
        || !isOwnedWorkspace(root, workspacePath)) {
        error = QStringLiteral(
            "Invalid OpenData temporary workspace");
        return {};
    }
    const QString canonicalWorkspace =
        QDir::fromNativeSeparators(
            QDir::cleanPath(workspacePath));
    WindowsPrivateSecurity security(false);
    if (canonicalWorkspace.isEmpty()
        || windowsHandlePath(
               workspaceGuard->handle())
            != canonicalWorkspace
        || !windowsHandleHasPrivateSecurity(
            workspaceGuard->handle(), true)
        || !security.attributes()) {
        error = QStringLiteral(
            "Cannot secure an OpenData temporary file");
        return {};
    }

    for (int attempt = 0; attempt < 10; ++attempt) {
        QString leafName = fileTemplate;
        leafName.replace(
            QStringLiteral("XXXXXX"),
            QUuid::createUuid()
                .toString(QUuid::WithoutBraces)
                .remove(QLatin1Char('-'))
                .left(12));
        const QString path =
            QDir(workspacePath).filePath(leafName);
        WindowsHandle handle(
            CreateFileW(
                reinterpret_cast<LPCWSTR>(path.utf16()),
                GENERIC_READ
                    | GENERIC_WRITE
                    | READ_CONTROL
                    | DELETE,
                0,
                security.attributes(),
                CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY
                    | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS
                || GetLastError() == ERROR_ALREADY_EXISTS) {
                continue;
            }
            error = QStringLiteral(
                "Cannot create a private OpenData temporary file");
            return {};
        }
        if (!setWindowsHandleDeletion(
                handle.get(), true)) {
            error = QStringLiteral(
                "Cannot secure OpenData temporary file cleanup");
            return {};
        }

        const QString openedPath =
            windowsHandlePath(handle.get());
        const QString openedParent =
            QFileInfo(openedPath).dir().absolutePath();
        const QString expectedPath =
            QDir::fromNativeSeparators(
                QDir::cleanPath(path));
        if (openedPath != expectedPath
            || openedParent != canonicalWorkspace
            || !windowsHandleHasPrivateSecurity(
                handle.get(), false)) {
            error = QStringLiteral(
                "Cannot securely create an OpenData temporary file");
            return {};
        }

        const HANDLE rawHandle = handle.release();
        const int descriptor =
            _open_osfhandle(
                reinterpret_cast<intptr_t>(rawHandle),
                _O_RDWR | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(rawHandle);
            error = QStringLiteral(
                "Cannot access a private OpenData temporary file");
            return {};
        }
        auto file = std::make_unique<QFile>(path);
        if (!file->open(
                descriptor,
                QIODevice::ReadWrite,
                QFileDevice::AutoCloseHandle)) {
            _close(descriptor);
            error = QStringLiteral(
                "Cannot access a private OpenData temporary file");
            return {};
        }
        const intptr_t nativeHandle =
            _get_osfhandle(descriptor);
        if (nativeHandle == -1
            || !setWindowsHandleDeletion(
                reinterpret_cast<HANDLE>(
                    nativeHandle),
                false)) {
            file->close();
            error = QStringLiteral(
                "Cannot preserve a validated OpenData temporary file");
            return {};
        }
        return std::make_unique<
            OpenDataTemporaryArchive::TemporaryFile>(
                path, std::move(file));
    }

    error = QStringLiteral(
        "Cannot allocate a unique OpenData temporary file");
    return {};
}

bool removeWindowsWorkspaceFile(
    const QString &workspacePath,
    const QFileInfo &entry,
    QString &error)
{
    const QString path =
        QDir::fromNativeSeparators(
            QDir::cleanPath(
                entry.absoluteFilePath()));
    WindowsHandle handle(
        CreateFileW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            FILE_READ_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    BY_HANDLE_FILE_INFORMATION information {};
    const QString openedPath =
        windowsHandlePath(handle.get());
    if (handle.get() == INVALID_HANDLE_VALUE
        || !GetFileInformationByHandle(
            handle.get(), &information)
        || information.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY
               | FILE_ATTRIBUTE_REPARSE_POINT)
        || openedPath != path
        || QFileInfo(openedPath)
               .dir()
               .absolutePath()
            != workspacePath
        || !setWindowsHandleDeletion(
            handle.get(), true)) {
        error = QStringLiteral(
            "Cannot securely remove OpenData temporary workspace file");
        return false;
    }
    return true;
}

bool removeWorkspace(
    const QString &root,
    const QString &path,
    OpenDataTemporaryArchive::WorkspaceGuard *workspaceGuard,
    QString &error)
{
    error.clear();
    if (!isOwnedWorkspace(root, path)) {
        error = QStringLiteral(
            "Invalid OpenData temporary workspace");
        return false;
    }

    WindowsHandle openedWorkspace;
    HANDLE workspaceHandle = INVALID_HANDLE_VALUE;
    if (workspaceGuard) {
        if (!workspaceGuard->isValid()) {
            error = QStringLiteral(
                "Invalid OpenData temporary workspace handle");
            return false;
        }
        workspaceHandle = workspaceGuard->handle();
    } else {
        openedWorkspace.reset(
            CreateFileW(
                reinterpret_cast<LPCWSTR>(path.utf16()),
                FILE_LIST_DIRECTORY
                    | FILE_READ_ATTRIBUTES
                    | READ_CONTROL
                    | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS
                    | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
        workspaceHandle = openedWorkspace.get();
    }

    BY_HANDLE_FILE_INFORMATION information {};
    const QString canonicalWorkspace =
        QDir::fromNativeSeparators(
            QDir::cleanPath(path));
    if (workspaceHandle == INVALID_HANDLE_VALUE
        || !GetFileInformationByHandle(
            workspaceHandle, &information)
        || !(information.dwFileAttributes
             & FILE_ATTRIBUTE_DIRECTORY)
        || information.dwFileAttributes
            & FILE_ATTRIBUTE_REPARSE_POINT
        || windowsHandlePath(workspaceHandle)
            != canonicalWorkspace
        || !windowsHandleHasPrivateSecurity(
            workspaceHandle, true)) {
        error = QStringLiteral(
            "Cannot securely open the OpenData temporary workspace");
        return false;
    }

    const QDir workspace(path);
    const QFileInfoList entries =
        workspace.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot,
            QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()
            || isReparsePoint(entry.absoluteFilePath())) {
            error = QStringLiteral(
                "Refusing to traverse an OpenData temporary workspace");
            return false;
        }
    }
    for (const QFileInfo &entry : entries) {
        if (!removeWindowsWorkspaceFile(
                canonicalWorkspace,
                entry,
                error)) {
            return false;
        }
    }
    if (!setWindowsHandleDeletion(
            workspaceHandle, true)) {
        error = QStringLiteral(
            "Cannot remove OpenData temporary workspace");
        return false;
    }
    return true;
}
#else
std::unique_ptr<OpenDataTemporaryArchive::TemporaryFile>
createWorkspaceFile(
    const QString &,
    const QString &,
    OpenDataTemporaryArchive::WorkspaceGuard *,
    const QString &,
    QString &error)
{
    error = QStringLiteral(
        "Secure OpenData temporary files are unavailable");
    return {};
}

bool removeWorkspace(
    const QString &,
    const QString &,
    OpenDataTemporaryArchive::WorkspaceGuard *,
    QString &error)
{
    error = QStringLiteral(
        "Secure OpenData workspace removal is unavailable");
    return false;
}
#endif

} // namespace

namespace OpenDataTemporaryArchive {

TemporaryFile::TemporaryFile(
    QString path,
    std::unique_ptr<QFile> device)
    : path_(std::move(path))
    , device_(std::move(device))
{
}

TemporaryFile::~TemporaryFile() = default;

QString TemporaryFile::path() const
{
    return path_;
}

QFile *TemporaryFile::device() const
{
    return device_.get();
}

Lease::Lease(
    QString root,
    QString directory,
    QString path,
    std::unique_ptr<QLockFile> lock,
    std::unique_ptr<WorkspaceGuard> workspaceGuard)
    : root_(std::move(root))
    , directory_(std::move(directory))
    , path_(std::move(path))
    , lock_(std::move(lock))
    , workspaceGuard_(std::move(workspaceGuard))
{
}

std::shared_ptr<Lease> Lease::create(
    const QString &directory,
    QString &error)
{
    error.clear();
    const QString root = canonicalDirectory(directory);
    if (root.isEmpty()) {
        error = QStringLiteral(
            "Invalid OpenData temporary directory");
        return {};
    }

    const QString workspaceName =
        QStringLiteral("gc-opendata-%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QUuid::createUuid().toString(
                QUuid::WithoutBraces));
    const QString workspace =
        QDir(root).filePath(workspaceName);
    auto lock =
        std::make_unique<QLockFile>(lockPath(workspace));
    lock->setStaleLockTime(
        WorkspaceLockStaleTimeMs);
    if (!lock->tryLock(0)) {
        error = QStringLiteral(
            "Cannot lock the OpenData temporary workspace");
        return {};
    }
    std::unique_ptr<WorkspaceGuard> workspaceGuard;
    if (!createPrivateWorkspace(
            root,
            workspaceName,
            workspaceGuard,
            error)) {
        return {};
    }
    const std::shared_ptr<Lease> lease(
        new Lease(
            root,
            workspace,
            {},
            std::move(lock),
            std::move(workspaceGuard)));
    std::unique_ptr<TemporaryFile> archive =
        lease->createFile(
            QStringLiteral("payload-XXXXXX.zip"),
            error);
    if (!archive) return {};
    archive->device()->close();
    lease->path_ = archive->path();
    return lease;
}

Lease::~Lease()
{
    QString error;
    if (!remove(error))
        qWarning().noquote() << error;
}

QString Lease::path() const
{
    return path_;
}

QString Lease::directoryPath() const
{
    return directory_;
}

std::unique_ptr<TemporaryFile> Lease::createFile(
    const QString &fileTemplate,
    QString &error) const
{
    error.clear();
    const QFileInfo templateInfo(fileTemplate);
    if (!isOwnedWorkspace(root_, directory_)
        || fileTemplate.isEmpty()
        || fileTemplate.contains(QLatin1Char('/'))
        || fileTemplate.contains(QLatin1Char('\\'))
        || !fileTemplate.contains(
            QStringLiteral("XXXXXX"))
        || templateInfo.isAbsolute()
        || templateInfo.fileName() != fileTemplate) {
        error = QStringLiteral(
            "Invalid OpenData temporary file template");
        return {};
    }

    return createWorkspaceFile(
        root_,
        directory_,
        workspaceGuard_.get(),
        fileTemplate,
        error);
}

bool Lease::remove(QString &error)
{
    error.clear();
    if (directory_.isEmpty()) return true;
    if (!removeWorkspace(
            root_,
            directory_,
            workspaceGuard_.get(),
            error)) {
        return false;
    }
    workspaceGuard_.reset();
    if (lock_) lock_->unlock();
    lock_.reset();
    root_.clear();
    directory_.clear();
    path_.clear();
    return true;
}

int removeAbandoned(
    const QString &directory,
    QStringList &errors)
{
    errors.clear();
    const QString root = canonicalDirectory(directory);
    if (root.isEmpty()) {
        errors.append(QStringLiteral(
            "Invalid OpenData temporary directory"));
        return 0;
    }
    const QDir temporaryDirectory(root);

    int removed = 0;
    const QFileInfoList candidates =
        temporaryDirectory.entryInfoList(
            {QStringLiteral("gc-opendata-*")},
            QDir::Dirs
                | QDir::NoDotAndDotDot,
            QDir::Name);
    for (const QFileInfo &candidate : candidates) {
        if (!isOwnedWorkspace(
                root, candidate.absoluteFilePath())) {
            errors.append(QStringLiteral(
                "Refusing invalid OpenData temporary workspace %1")
                              .arg(candidate.fileName()));
            continue;
        }

        QLockFile lock(lockPath(candidate.absoluteFilePath()));
        lock.setStaleLockTime(
            WorkspaceLockStaleTimeMs);
        if (!lock.tryLock(0)) {
            if (lock.error() != QLockFile::LockFailedError) {
                errors.append(QStringLiteral(
                    "Cannot inspect OpenData temporary workspace %1")
                                  .arg(candidate.fileName()));
            }
            continue;
        }

        QString removalError;
        if (removeWorkspace(
                root,
                candidate.absoluteFilePath(),
                nullptr,
                removalError)) {
            lock.unlock();
            ++removed;
        } else {
            errors.append(QStringLiteral(
                "%1: %2")
                              .arg(
                                  candidate.fileName(),
                                  removalError));
        }
    }

    const QFileInfoList orphanLocks =
        temporaryDirectory.entryInfoList(
            {QStringLiteral("gc-opendata-*.lock")},
            QDir::Files
                | QDir::NoSymLinks
                | QDir::NoDotAndDotDot,
            QDir::Name);
    for (const QFileInfo &orphan : orphanLocks) {
        QString workspacePath =
            orphan.absoluteFilePath();
        workspacePath.chop(
            QStringLiteral(".lock").size());
        if (QFileInfo::exists(workspacePath))
            continue;
        QLockFile lock(orphan.absoluteFilePath());
        lock.setStaleLockTime(
            WorkspaceLockStaleTimeMs);
        if (lock.tryLock(0)) {
            lock.unlock();
        } else if (lock.error()
                   != QLockFile::LockFailedError) {
            errors.append(QStringLiteral(
                "Cannot inspect orphaned OpenData lock %1")
                              .arg(orphan.fileName()));
        }
    }
    return removed;
}

#ifdef GC_OPEN_DATA_TEMPORARY_ARCHIVE_TEST_HOOKS
bool createPrivateWorkspaceForTest(
    const QString &directory,
    const QString &workspaceName,
    QString &error)
{
    error.clear();
    const QString root =
        canonicalDirectory(directory);
    const QFileInfo nameInfo(workspaceName);
    if (root.isEmpty()
        || !workspaceName.startsWith(
            QStringLiteral("gc-opendata-"))
        || nameInfo.isAbsolute()
        || nameInfo.fileName() != workspaceName) {
        error = QStringLiteral(
            "Invalid OpenData temporary workspace");
        return false;
    }
    std::unique_ptr<WorkspaceGuard> guard;
    return createPrivateWorkspace(
        root,
        workspaceName,
        guard,
        error);
}

bool rejectPrivateWorkspaceAfterCreateForTest(
    const QString &directory,
    const QString &workspaceName,
    QString &error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(directory)
    Q_UNUSED(workspaceName)
    error = QStringLiteral(
        "Windows post-create validation is unavailable");
    return false;
#else
    error.clear();
    const QString root =
        canonicalDirectory(directory);
    const QFileInfo nameInfo(workspaceName);
    if (root.isEmpty()
        || !workspaceName.startsWith(
            QStringLiteral("gc-opendata-"))
        || nameInfo.isAbsolute()
        || nameInfo.fileName() != workspaceName) {
        error = QStringLiteral(
            "Invalid OpenData temporary workspace");
        return false;
    }
    std::unique_ptr<WorkspaceGuard> guard;
    return createPrivateWorkspace(
        root,
        workspaceName,
        guard,
        error,
        true);
#endif
}

bool hasPrivateSecurityForTest(
    const QString &path,
    bool directory)
{
#ifdef Q_OS_WIN
    WindowsHandle handle(
        CreateFileW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ
                | FILE_SHARE_WRITE
                | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT
                | (directory
                    ? FILE_FLAG_BACKUP_SEMANTICS
                    : 0),
            nullptr));
    return handle.get() != INVALID_HANDLE_VALUE
        && windowsHandlePath(handle.get())
            == QDir::fromNativeSeparators(
                QDir::cleanPath(path))
        && windowsHandleHasPrivateSecurity(
            handle.get(), directory);
#else
    Q_UNUSED(path)
    Q_UNUSED(directory)
    return false;
#endif
}
#endif

} // namespace OpenDataTemporaryArchive
