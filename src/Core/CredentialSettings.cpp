#include "CredentialSettings.h"

#include "AtomicFileWriter.h"
#include "Settings.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>
#include <aclapi.h>
#endif

namespace {

bool credentialDirectoryTreeIsDurable(
    const QString &directoryPath,
    const QString &existingAncestorPath,
    bool force);

#ifdef Q_OS_WIN
bool windowsCredentialRootIsSafe(const QString &path);
bool windowsCredentialPathHasNoReparseComponents(
    const QString &path);
#endif

QMutex &credentialOperationMutex()
{
    static QMutex mutex;
    return mutex;
}

bool credentialRootIsSecure(
    const QFileInfo &directory)
{
    if (!directory.isDir() || directory.isSymLink())
        return false;
#ifdef Q_OS_UNIX
    QString current =
        QDir::cleanPath(directory.absoluteFilePath());
    bool root = true;
    while (true) {
        struct stat information;
        const QByteArray encoded = QFile::encodeName(current);
        if (::lstat(encoded.constData(), &information) != 0
            || !S_ISDIR(information.st_mode)) {
            return false;
        }
        const mode_t writableByOthers =
            information.st_mode & (S_IWGRP | S_IWOTH);
        if (root) {
            if (information.st_uid != ::geteuid()
                || writableByOthers != 0) {
                return false;
            }
            root = false;
        } else if (writableByOthers != 0
                   && !(information.st_mode & S_ISVTX)) {
            return false;
        }
        const QString parent =
            QFileInfo(current).absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
#elif defined(Q_OS_WIN)
    const QString path =
        QDir::cleanPath(directory.absoluteFilePath());
    if (!windowsCredentialRootIsSafe(path)
        || !windowsCredentialPathHasNoReparseComponents(
            path)) {
        return false;
    }
#endif
    return true;
}

bool credentialPathIsRedirected(
    const QFileInfo &information)
{
    if (information.isSymLink())
        return true;
#ifdef Q_OS_WIN
    const QString path = information.absoluteFilePath();
    const DWORD attributes = ::GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}

#ifdef Q_OS_WIN
class ScopedWindowsHandle
{
public:
    explicit ScopedWindowsHandle(
        HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~ScopedWindowsHandle()
    {
        if (isValid())
            ::CloseHandle(handle_);
    }

    ScopedWindowsHandle(ScopedWindowsHandle &&other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    ScopedWindowsHandle &operator=(
        ScopedWindowsHandle &&other) noexcept
    {
        if (this == &other)
            return *this;
        if (isValid())
            ::CloseHandle(handle_);
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
        return *this;
    }

    bool isValid() const
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

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

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    Q_DISABLE_COPY(ScopedWindowsHandle)
};

ScopedWindowsHandle openWindowsCredentialDirectory(
    const QString &path,
    DWORD access,
    bool shareDelete = true,
    bool openReparsePoint = true)
{
    DWORD sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE;
    if (shareDelete)
        sharing |= FILE_SHARE_DELETE;
    return ScopedWindowsHandle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        access,
        sharing,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | (openReparsePoint
                   ? FILE_FLAG_OPEN_REPARSE_POINT
                   : DWORD(0)),
        nullptr));
}

ScopedWindowsHandle openWindowsCredentialFile(
    const QString &path,
    DWORD access)
{
    return ScopedWindowsHandle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE
            | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL
            | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
}

bool windowsHandleIsUnredirectedDirectory(
    HANDLE handle)
{
    FILE_ATTRIBUTE_TAG_INFO information = {};
    return handle
        && handle != INVALID_HANDLE_VALUE
        && ::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo,
            &information, sizeof(information))
        && (information.FileAttributes
            & FILE_ATTRIBUTE_DIRECTORY)
        && !(information.FileAttributes
             & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool windowsHandleIsUnredirectedFile(HANDLE handle)
{
    FILE_ATTRIBUTE_TAG_INFO information = {};
    return handle
        && handle != INVALID_HANDLE_VALUE
        && ::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo,
            &information, sizeof(information))
        && !(information.FileAttributes
             & FILE_ATTRIBUTE_DIRECTORY)
        && !(information.FileAttributes
             & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool windowsHandleSupportsPersistentAcls(HANDLE handle)
{
    DWORD fileSystemFlags = 0;
    return handle
        && handle != INVALID_HANDLE_VALUE
        && ::GetVolumeInformationByHandleW(
            handle,
            nullptr, 0,
            nullptr, nullptr,
            &fileSystemFlags,
            nullptr, 0)
        && (fileSystemFlags & FILE_PERSISTENT_ACLS);
}

QString windowsPathForHandle(HANDLE handle)
{
    const DWORD flags =
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required =
        ::GetFinalPathNameByHandleW(
            handle, nullptr, 0, flags);
    if (required == 0)
        return {};
    QString path;
    path.resize(int(required));
    const DWORD written =
        ::GetFinalPathNameByHandleW(
            handle,
            reinterpret_cast<LPWSTR>(path.data()),
            required, flags);
    if (written == 0 || written >= required)
        return {};
    path.resize(int(written));
    if (path.startsWith(
            QStringLiteral("\\\\?\\UNC\\"))) {
        path = QStringLiteral("\\\\") + path.mid(8);
    } else if (path.startsWith(
                   QStringLiteral("\\\\?\\"))) {
        path.remove(0, 4);
    }
    return QDir::cleanPath(path);
}

bool currentWindowsUserSid(
    QByteArray *storage,
    PSID *sid)
{
    if (!storage || !sid)
        return false;
    ScopedWindowsHandle token;
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(),
            TOKEN_QUERY, &rawToken)) {
        return false;
    }
    token = ScopedWindowsHandle(rawToken);

    DWORD required = 0;
    ::GetTokenInformation(
        token.get(), TokenUser,
        nullptr, 0, &required);
    if (::GetLastError()
            != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        return false;
    }
    storage->resize(int(required));
    if (!::GetTokenInformation(
            token.get(), TokenUser,
            storage->data(), required, &required)) {
        return false;
    }
    auto *user = reinterpret_cast<TOKEN_USER *>(
        storage->data());
    if (!::IsValidSid(user->User.Sid))
        return false;
    *sid = user->User.Sid;
    return true;
}

bool windowsWellKnownSid(
    WELL_KNOWN_SID_TYPE type,
    QByteArray *storage,
    PSID *sid)
{
    if (!storage || !sid)
        return false;
    storage->resize(SECURITY_MAX_SID_SIZE);
    DWORD size = DWORD(storage->size());
    if (!::CreateWellKnownSid(
            type, nullptr, storage->data(), &size)) {
        return false;
    }
    storage->resize(int(size));
    *sid = storage->data();
    return ::IsValidSid(*sid);
}

bool windowsCurrentUserAcl(
    QByteArray *userStorage,
    QByteArray *aclStorage,
    PSID *userSid,
    PACL *acl)
{
    if (!userStorage || !aclStorage
        || !userSid || !acl
        || !currentWindowsUserSid(
            userStorage, userSid)) {
        return false;
    }
    const DWORD aclSize = DWORD(
        sizeof(ACL)
        + sizeof(ACCESS_ALLOWED_ACE)
        - sizeof(DWORD)
        + ::GetLengthSid(*userSid));
    aclStorage->resize(int(aclSize));
    *acl = reinterpret_cast<PACL>(
        aclStorage->data());
    return ::InitializeAcl(
               *acl, aclSize, ACL_REVISION)
        && ::AddAccessAllowedAceEx(
            *acl, ACL_REVISION,
            CONTAINER_INHERIT_ACE
                | OBJECT_INHERIT_ACE,
            FILE_ALL_ACCESS, *userSid);
}

bool windowsPrivateSecurityAttributes(
    QByteArray *userStorage,
    QByteArray *aclStorage,
    SECURITY_DESCRIPTOR *descriptor,
    SECURITY_ATTRIBUTES *attributes)
{
    if (!descriptor || !attributes)
        return false;
    PSID userSid = nullptr;
    PACL acl = nullptr;
    if (!windowsCurrentUserAcl(
            userStorage, aclStorage,
            &userSid, &acl)) {
        return false;
    }
    if (!::InitializeSecurityDescriptor(
            descriptor,
            SECURITY_DESCRIPTOR_REVISION)
        || !::SetSecurityDescriptorOwner(
            descriptor, userSid, FALSE)
        || !::SetSecurityDescriptorDacl(
            descriptor, TRUE, acl, FALSE)
        || !::SetSecurityDescriptorControl(
            descriptor,
            SE_DACL_PROTECTED,
            SE_DACL_PROTECTED)) {
        return false;
    }
    attributes->nLength = sizeof(*attributes);
    attributes->lpSecurityDescriptor = descriptor;
    attributes->bInheritHandle = FALSE;
    return true;
}

bool windowsIsAllowedAceType(BYTE type)
{
    return type == ACCESS_ALLOWED_ACE_TYPE
        || type == ACCESS_ALLOWED_OBJECT_ACE_TYPE
        || type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
        || type
            == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
}

ACCESS_MASK windowsAceMask(
    const ACE_HEADER *header)
{
    ACCESS_MASK mask = 0;
    if (!header
        || header->AceSize
            < sizeof(ACE_HEADER)
                + sizeof(ACCESS_MASK)) {
        return mask;
    }
    std::memcpy(
        &mask,
        reinterpret_cast<const BYTE *>(header)
            + sizeof(ACE_HEADER),
        sizeof(mask));
    return mask;
}

ACCESS_MASK windowsMappedFileMask(
    ACCESS_MASK mask)
{
    GENERIC_MAPPING mapping = {
        FILE_GENERIC_READ,
        FILE_GENERIC_WRITE,
        FILE_GENERIC_EXECUTE,
        FILE_ALL_ACCESS
    };
    ::MapGenericMask(&mask, &mapping);
    return mask;
}

bool windowsSidIsTrusted(
    PSID sid,
    PSID userSid,
    PSID systemSid,
    PSID administratorsSid)
{
    return sid && ::IsValidSid(sid)
        && ((userSid && ::EqualSid(sid, userSid))
            || (systemSid
                && ::EqualSid(sid, systemSid))
            || (administratorsSid
                && ::EqualSid(
                    sid, administratorsSid)));
}

bool windowsCredentialRootHandleIsSafe(HANDLE handle)
{
    if (!windowsHandleIsUnredirectedDirectory(handle)
        || !windowsHandleSupportsPersistentAcls(
            handle)) {
        return false;
    }

    QByteArray userStorage;
    QByteArray systemStorage;
    QByteArray administratorsStorage;
    PSID userSid = nullptr;
    PSID systemSid = nullptr;
    PSID administratorsSid = nullptr;
    if (!currentWindowsUserSid(
            &userStorage, &userSid)
        || !windowsWellKnownSid(
            WinLocalSystemSid,
            &systemStorage, &systemSid)
        || !windowsWellKnownSid(
            WinBuiltinAdministratorsSid,
            &administratorsStorage,
            &administratorsSid)) {
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION
            | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS)
        return false;
    bool safe = windowsSidIsTrusted(
                    owner, userSid, systemSid,
                    administratorsSid)
        && acl;
    bool userCanCreate = false;
    constexpr ACCESS_MASK dangerousAccess =
        FILE_WRITE_DATA
        | FILE_APPEND_DATA
        | FILE_WRITE_EA
        | FILE_WRITE_ATTRIBUTES
        | FILE_DELETE_CHILD
        | DELETE
        | WRITE_DAC
        | WRITE_OWNER
        | GENERIC_WRITE
        | GENERIC_ALL
        | MAXIMUM_ALLOWED;
    if (safe) {
        for (DWORD index = 0;
             index < acl->AceCount; ++index) {
            void *rawAce = nullptr;
            if (!::GetAce(acl, index, &rawAce)) {
                safe = false;
                break;
            }
            auto *header =
                static_cast<ACE_HEADER *>(rawAce);
            if (!windowsIsAllowedAceType(
                    header->AceType)
                || (header->AceFlags
                    & INHERIT_ONLY_ACE)) {
                continue;
            }
            const ACCESS_MASK mask =
                windowsMappedFileMask(
                    windowsAceMask(header));
            if (header->AceType
                != ACCESS_ALLOWED_ACE_TYPE) {
                if (mask & dangerousAccess) {
                    safe = false;
                    break;
                }
                continue;
            }
            auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(
                    rawAce);
            const bool currentUser =
                ::IsValidSid(&ace->SidStart)
                && ::EqualSid(
                    &ace->SidStart, userSid);
            if (currentUser
                && (mask
                    & FILE_ADD_SUBDIRECTORY)) {
                userCanCreate = true;
            }
            if ((mask & dangerousAccess)
                && !windowsSidIsTrusted(
                    &ace->SidStart,
                    userSid, systemSid,
                    administratorsSid)) {
                safe = false;
                break;
            }
        }
    }
    if (descriptor)
        ::LocalFree(descriptor);
    return safe && userCanCreate;
}

bool windowsCredentialRootIsSafe(
    const QString &path)
{
    ScopedWindowsHandle directory =
        openWindowsCredentialDirectory(
            path,
            FILE_READ_ATTRIBUTES | READ_CONTROL);
    return directory.isValid()
        && windowsCredentialRootHandleIsSafe(
            directory.get());
}

bool windowsCredentialPathHasNoReparseComponents(
    const QString &path)
{
    QString current =
        QDir::cleanPath(
            QFileInfo(path).absoluteFilePath());
    while (true) {
        ScopedWindowsHandle directory =
            openWindowsCredentialDirectory(
                current, FILE_READ_ATTRIBUTES);
        if (!directory.isValid()
            || !windowsHandleIsUnredirectedDirectory(
                directory.get())) {
            return false;
        }
        const QString parent =
            QFileInfo(current).absolutePath();
        if (parent == current)
            return true;
        current = parent;
    }
}

bool windowsCredentialDirectoryIsSafe(
    const QString &path)
{
    ScopedWindowsHandle directory =
        openWindowsCredentialDirectory(
            path, FILE_READ_ATTRIBUTES);
    return directory.isValid()
        && windowsHandleIsUnredirectedDirectory(
            directory.get());
}

bool windowsHandleIsPrivateCredentialDirectory(
    HANDLE handle)
{
    if (!windowsHandleIsUnredirectedDirectory(handle)
        || !windowsHandleSupportsPersistentAcls(handle)) {
        return false;
    }

    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsUserSid(
            &userStorage, &userSid)) {
        return false;
    }
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION
            | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS)
        return false;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool safe = owner && ::EqualSid(owner, userSid)
        && acl
        && ::GetSecurityDescriptorControl(
            descriptor, &control, &revision)
        && (control & SE_DACL_PROTECTED);
    bool userAllowed = false;
    DWORD allowedAceCount = 0;
    if (safe) {
        for (DWORD index = 0;
             index < acl->AceCount; ++index) {
            void *rawAce = nullptr;
            if (!::GetAce(acl, index, &rawAce)) {
                safe = false;
                break;
            }
            auto *header =
                static_cast<ACE_HEADER *>(rawAce);
            if (header->AceFlags & INHERITED_ACE) {
                safe = false;
                break;
            }
            if (!windowsIsAllowedAceType(
                    header->AceType)) {
                continue;
            }
            ++allowedAceCount;
            if (header->AceType
                != ACCESS_ALLOWED_ACE_TYPE) {
                safe = false;
                break;
            }
            auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(
                    rawAce);
            if (!::IsValidSid(&ace->SidStart)
                || !::EqualSid(
                    &ace->SidStart, userSid)
                || (header->AceFlags
                    & (OBJECT_INHERIT_ACE
                       | CONTAINER_INHERIT_ACE))
                    != (OBJECT_INHERIT_ACE
                        | CONTAINER_INHERIT_ACE)
                || (header->AceFlags
                    & (INHERIT_ONLY_ACE
                       | NO_PROPAGATE_INHERIT_ACE))
                || (ace->Mask & FILE_ALL_ACCESS)
                    != FILE_ALL_ACCESS) {
                safe = false;
                break;
            }
            userAllowed = true;
        }
    }
    if (descriptor)
        ::LocalFree(descriptor);
    return safe && userAllowed
        && allowedAceCount == 1;
}

bool validateWindowsPrivateCredentialDirectory(
    const QString &path,
    QString *stablePath = nullptr)
{
    ScopedWindowsHandle directory =
        openWindowsCredentialDirectory(
            path,
            FILE_READ_ATTRIBUTES | READ_CONTROL);
    if (!directory.isValid()
        || !windowsHandleIsPrivateCredentialDirectory(
            directory.get())) {
        return false;
    }
    if (stablePath) {
        *stablePath =
            windowsPathForHandle(directory.get());
        if (stablePath->isEmpty())
            return false;
    }
    return true;
}

bool createWindowsCredentialDirectory(
    const QString &parentPath,
    const QString &name,
    bool *created)
{
    const QString path = QDir(parentPath).filePath(name);
    const DWORD existingAttributes = ::GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(path.utf16()));
    if (existingAttributes != INVALID_FILE_ATTRIBUTES) {
        if (created) *created = false;
        return validateWindowsPrivateCredentialDirectory(
            path);
    }
    const DWORD lookupError = ::GetLastError();
    if (lookupError != ERROR_FILE_NOT_FOUND
        && lookupError != ERROR_PATH_NOT_FOUND) {
        return false;
    }
    if (!windowsCredentialDirectoryIsSafe(parentPath))
        return false;

    QByteArray userStorage;
    QByteArray aclStorage;
    SECURITY_DESCRIPTOR descriptor = {};
    SECURITY_ATTRIBUTES attributes = {};
    if (!windowsPrivateSecurityAttributes(
            &userStorage, &aclStorage,
            &descriptor, &attributes)) {
        return false;
    }

    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString temporaryName =
            QStringLiteral(".%1.%2.tmp")
                .arg(name,
                     QUuid::createUuid().toString(
                         QUuid::WithoutBraces));
        const QString temporaryPath =
            QDir(parentPath).filePath(temporaryName);
        if (!::CreateDirectoryW(
                reinterpret_cast<LPCWSTR>(
                    temporaryPath.utf16()),
                &attributes)) {
            const DWORD createError = ::GetLastError();
            if (createError == ERROR_ALREADY_EXISTS
                || createError == ERROR_FILE_EXISTS) {
                continue;
            }
            return false;
        }
        if (!validateWindowsPrivateCredentialDirectory(
                temporaryPath)) {
            ::RemoveDirectoryW(
                reinterpret_cast<LPCWSTR>(
                    temporaryPath.utf16()));
            return false;
        }

        if (::MoveFileExW(
                reinterpret_cast<LPCWSTR>(
                    temporaryPath.utf16()),
                reinterpret_cast<LPCWSTR>(path.utf16()),
                MOVEFILE_WRITE_THROUGH)) {
            if (created) *created = true;
            return validateWindowsPrivateCredentialDirectory(
                path);
        }

        const DWORD moveError = ::GetLastError();
        ::RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(
                temporaryPath.utf16()));
        if (moveError == ERROR_ALREADY_EXISTS
            || moveError == ERROR_FILE_EXISTS) {
            if (created) *created = false;
            return validateWindowsPrivateCredentialDirectory(
                path);
        }
        return false;
    }
    return false;
}

bool ensureWindowsCredentialDirectoryPath(
    const QString &path,
    QString *stablePath,
    bool *created,
    ScopedWindowsHandle *retainedHandle,
    bool allowRedirectedBoundary)
{
    if (!retainedHandle)
        return false;
    QString current = QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
    QStringList missingNames;
    while (!QFileInfo::exists(current)) {
        const QFileInfo information(current);
        const QString name = information.fileName();
        const QString parent = information.absolutePath();
        if (name.isEmpty() || parent == current)
            return false;
        missingNames.prepend(name);
        current = parent;
    }
    if (allowRedirectedBoundary) {
        if (!missingNames.isEmpty())
            return false;
        ScopedWindowsHandle retained =
            openWindowsCredentialDirectory(
                current,
                FILE_READ_ATTRIBUTES | READ_CONTROL,
                false, false);
        if (!retained.isValid()
            || !windowsCredentialRootHandleIsSafe(
                retained.get())) {
            return false;
        }
        const QString canonical =
            windowsPathForHandle(retained.get());
        if (canonical.isEmpty())
            return false;
        if (stablePath) *stablePath = canonical;
        if (created) *created = false;
        *retainedHandle = std::move(retained);
        return true;
    }
    if (!credentialRootIsSecure(QFileInfo(current)))
        return false;

    bool anyCreated = false;
    for (const QString &name : std::as_const(missingNames)) {
        bool componentCreated = false;
        if (!createWindowsCredentialDirectory(
                current, name, &componentCreated)) {
            return false;
        }
        anyCreated = anyCreated || componentCreated;
        current = QDir(current).filePath(name);
    }
    ScopedWindowsHandle retained =
        openWindowsCredentialDirectory(
            current,
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            false);
    if (!retained.isValid()
        || !windowsCredentialRootHandleIsSafe(
            retained.get())
        || !windowsCredentialPathHasNoReparseComponents(
            current)) {
        return false;
    }
    const QString canonical =
        windowsPathForHandle(retained.get());
    if (canonical.isEmpty())
        return false;
    if (stablePath) *stablePath = canonical;
    if (created) *created = anyCreated;
    *retainedHandle = std::move(retained);
    return true;
}
#endif

bool ensurePrivateCredentialDirectory(
    const QString &parentPath,
    const QString &name,
    QString *stablePath,
    bool *created)
{
    QDir parent(parentPath);
    const QString path = parent.filePath(name);
#ifdef Q_OS_WIN
    bool directoryCreated = false;
    if (!createWindowsCredentialDirectory(
            parentPath, name, &directoryCreated)) {
        return false;
    }
    const bool existed = !directoryCreated;
#else
    const bool existed = QFileInfo::exists(path);
    if (!existed && !parent.mkdir(name))
        return false;
#endif

    QString canonical;
#ifdef Q_OS_WIN
    if (!validateWindowsPrivateCredentialDirectory(
            path, &canonical)) {
        return false;
    }
#else
    const QFileInfo directory(path);
    if (!directory.isDir() || directory.isSymLink())
        return false;
#ifdef Q_OS_UNIX
    if (directory.ownerId() != uint(::geteuid())
        || !QFile::setPermissions(
            path,
            QFileDevice::ReadOwner
                | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        return false;
    }
    const QFileInfo hardened(path);
    const QFileDevice::Permissions forbidden =
        QFileDevice::ReadGroup
        | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup
        | QFileDevice::ReadOther
        | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    if (hardened.ownerId() != uint(::geteuid())
        || (hardened.permissions() & forbidden)
            != QFileDevice::Permissions()) {
        return false;
    }
#endif
    canonical = QFileInfo(path).canonicalFilePath();
#endif
    if (canonical.isEmpty())
        return false;
    if (stablePath) *stablePath = canonical;
    if (created) *created = !existed;
    return true;
}

#ifdef Q_OS_WIN
QString credentialStateDirectory(
    ScopedWindowsHandle *rootDirectoryHandle,
    ScopedWindowsHandle *applicationDirectoryHandle,
    ScopedWindowsHandle *lockDirectoryHandle)
#else
QString credentialStateDirectory()
#endif
{
#ifdef Q_OS_WIN
    if (!rootDirectoryHandle
        || !applicationDirectoryHandle
        || !lockDirectoryHandle) {
        return {};
    }
#endif
    QString root;
#ifdef Q_OS_WIN
    bool explicitRoot = false;
#endif
#ifdef GC_CREDENTIAL_STORE_CUSTOM_FACTORY
    root = qEnvironmentVariable(
        "GC_CREDENTIAL_TEST_STATE_ROOT");
#ifdef Q_OS_WIN
    explicitRoot = !root.isEmpty();
#endif
#endif
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation);
    }
    if (root.isEmpty())
        return {};

    const QString rootPath =
        QFileInfo(root).absoluteFilePath();
#ifdef Q_OS_WIN
    QString stableRoot;
    bool createdRoot = false;
    ScopedWindowsHandle retainedRoot;
    if (!ensureWindowsCredentialDirectoryPath(
            rootPath, &stableRoot, &createdRoot,
            &retainedRoot, !explicitRoot)) {
        return {};
    }
#else
    const bool createdRoot =
        !QFileInfo::exists(rootPath);
    if (!QDir().mkpath(rootPath))
        return {};
    if (createdRoot) {
#ifdef Q_OS_UNIX
        if (!QFile::setPermissions(
                rootPath,
                QFileDevice::ReadOwner
                    | QFileDevice::WriteOwner
                    | QFileDevice::ExeOwner)) {
            return {};
        }
#endif
    }
    const QFileInfo rootDirectory(rootPath);
    if (!credentialRootIsSecure(rootDirectory))
        return {};
    const QString stableRoot =
        rootDirectory.canonicalFilePath();
    if (stableRoot.isEmpty())
        return {};
#endif

    QString applicationPath;
    bool createdApplication = false;
    if (!ensurePrivateCredentialDirectory(
            stableRoot,
            QStringLiteral("GoldenCheetah"),
            &applicationPath,
            &createdApplication)) {
        return {};
    }
#ifdef Q_OS_WIN
    ScopedWindowsHandle retainedApplication =
        openWindowsCredentialDirectory(
            applicationPath,
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            false);
    if (!retainedApplication.isValid()
        || !windowsHandleIsPrivateCredentialDirectory(
            retainedApplication.get())) {
        return {};
    }
    applicationPath = windowsPathForHandle(
        retainedApplication.get());
    if (applicationPath.isEmpty())
        return {};
#endif
    QString stablePath;
    bool createdLocks = false;
    if (!ensurePrivateCredentialDirectory(
            applicationPath,
            QStringLiteral("credential-locks"),
            &stablePath,
            &createdLocks)) {
        return {};
    }
#ifdef Q_OS_WIN
    ScopedWindowsHandle retainedLocks =
        openWindowsCredentialDirectory(
            stablePath,
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            false);
    if (!retainedLocks.isValid()
        || !windowsHandleIsPrivateCredentialDirectory(
            retainedLocks.get())) {
        return {};
    }
    stablePath = windowsPathForHandle(
        retainedLocks.get());
    if (stablePath.isEmpty())
        return {};
#endif

    QString stableAncestor = stablePath;
    while (true) {
        const QString parent =
            QFileInfo(stableAncestor).absolutePath();
        if (parent == stableAncestor)
            break;
        stableAncestor = parent;
    }
    if (!credentialDirectoryTreeIsDurable(
            stablePath, stableAncestor,
            createdRoot || createdApplication
                || createdLocks)) {
        return {};
    }
#ifdef Q_OS_WIN
    *rootDirectoryHandle = std::move(retainedRoot);
    *applicationDirectoryHandle =
        std::move(retainedApplication);
    *lockDirectoryHandle = std::move(retainedLocks);
#endif
    return stablePath;
}

QString canonicalSettingsFileName(QSettings *settings)
{
    if (!settings || settings->fileName().isEmpty())
        return {};
#ifdef Q_OS_WIN
    if (settings->format() == QSettings::NativeFormat) {
        QString registryPath = settings->fileName().trimmed();
        registryPath.replace(
            QLatin1Char('/'), QLatin1Char('\\'));
        while (registryPath.contains(
                   QStringLiteral("\\\\"))) {
            registryPath.replace(
                QStringLiteral("\\\\"),
                QStringLiteral("\\"));
        }
        while (registryPath.size() > 1
               && registryPath.endsWith(
                   QLatin1Char('\\'))) {
            registryPath.chop(1);
        }
        return QStringLiteral("registry:")
            + registryPath.toCaseFolded();
    }
#endif
    const QFileInfo file(settings->fileName());
    const QString canonical = file.canonicalFilePath();
    if (!canonical.isEmpty())
        return QStringLiteral("file:") + canonical;

    const QFileInfo parent(file.absolutePath());
    const QString canonicalParent =
        parent.canonicalFilePath();
    if (!canonicalParent.isEmpty()) {
        return QStringLiteral("file:")
            + QDir(canonicalParent).filePath(
                file.fileName());
    }
    return QStringLiteral("file:")
        + QDir::cleanPath(file.absoluteFilePath());
}

class CredentialOperationGuard
{
public:
    explicit CredentialOperationGuard(
        const QString &operationId)
    {
        if (!credentialOperationMutex().tryLock())
            return;
        localLocked_ = true;

        const QString directory =
#ifdef Q_OS_WIN
            credentialStateDirectory(
                &rootDirectoryHandle_,
                &applicationDirectoryHandle_,
                &lockDirectoryHandle_);
#else
            credentialStateDirectory();
#endif
        if (directory.isEmpty())
            return;
        const QByteArray digest = QCryptographicHash::hash(
            operationId.toUtf8(),
            QCryptographicHash::Sha256).toHex();
        stateBasePath_ = QDir(directory).filePath(
            QString::fromLatin1(digest));
        processLock_ = std::make_unique<QLockFile>(
            stateBasePath_ + QStringLiteral(".lock"));
        processLock_->setStaleLockTime(0);
        if (!processLock_->tryLock(0))
            return;
        admitted_ = true;
    }

    ~CredentialOperationGuard()
    {
        if (processLock_ && processLock_->isLocked())
            processLock_->unlock();
        if (localLocked_)
            credentialOperationMutex().unlock();
    }

    explicit operator bool() const
    {
        return admitted_;
    }

    QString revisionPath() const
    {
        return stateBasePath_ + QStringLiteral(".revision");
    }

    QString deletionPath() const
    {
        return stateBasePath_ + QStringLiteral(".deletion");
    }

private:
    bool localLocked_ = false;
    bool admitted_ = false;
    QString stateBasePath_;
    std::unique_ptr<QLockFile> processLock_;
#ifdef Q_OS_WIN
    ScopedWindowsHandle rootDirectoryHandle_;
    ScopedWindowsHandle applicationDirectoryHandle_;
    ScopedWindowsHandle lockDirectoryHandle_;
#endif
    Q_DISABLE_COPY(CredentialOperationGuard)
};

struct CredentialRevision
{
    bool readable = false;
    QByteArray value;
};

enum class CredentialDeletionPhase
{
    Absent,
    PreparingCreation,
    Preparing,
    PreparingReplacement,
    Deleting,
    Deleted,
    Replacing,
    Creating,
    Updating,
    Active
};

bool isCredentialDeletionPreparation(
    CredentialDeletionPhase phase)
{
    return phase == CredentialDeletionPhase::PreparingCreation
        || phase == CredentialDeletionPhase::Preparing
        || phase
            == CredentialDeletionPhase::PreparingReplacement;
}

CredentialDeletionPhase credentialDeletionPreparationFor(
    CredentialDeletionPhase phase)
{
    if (phase == CredentialDeletionPhase::Absent
        || phase == CredentialDeletionPhase::Creating) {
        return CredentialDeletionPhase::PreparingCreation;
    }
    if (phase == CredentialDeletionPhase::Replacing)
        return CredentialDeletionPhase::PreparingReplacement;
    return CredentialDeletionPhase::Preparing;
}

CredentialDeletionPhase credentialWritePhaseFor(
    CredentialDeletionPhase phase)
{
    if (phase == CredentialDeletionPhase::Deleted
        || phase == CredentialDeletionPhase::Replacing
        || phase
            == CredentialDeletionPhase::PreparingReplacement) {
        return CredentialDeletionPhase::Replacing;
    }
    if (phase == CredentialDeletionPhase::Absent
        || phase == CredentialDeletionPhase::Creating
        || phase
            == CredentialDeletionPhase::PreparingCreation) {
        return CredentialDeletionPhase::Creating;
    }
    return CredentialDeletionPhase::Updating;
}

struct CredentialDeletionState
{
    bool readable = false;
    CredentialDeletionPhase phase =
        CredentialDeletionPhase::Absent;
    QByteArray transaction;
};

bool supersedesPendingRemoval(
    CredentialDeletionPhase phase)
{
    return phase == CredentialDeletionPhase::Replacing
        || phase == CredentialDeletionPhase::Creating
        || phase == CredentialDeletionPhase::Updating
        || phase == CredentialDeletionPhase::Active;
}

bool isCredentialTransaction(const QByteArray &value)
{
    if (value.size() != 32)
        return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

QByteArray newCredentialTransaction()
{
    return QUuid::createUuid().toRfc4122().toHex();
}

QByteArray credentialDeletionPhaseName(
    CredentialDeletionPhase phase)
{
    switch (phase) {
    case CredentialDeletionPhase::PreparingCreation:
        return QByteArrayLiteral("preparing-creation");
    case CredentialDeletionPhase::Preparing:
        return QByteArrayLiteral("preparing");
    case CredentialDeletionPhase::PreparingReplacement:
        return QByteArrayLiteral("preparing-replacement");
    case CredentialDeletionPhase::Deleting:
        return QByteArrayLiteral("deleting");
    case CredentialDeletionPhase::Deleted:
        return QByteArrayLiteral("deleted");
    case CredentialDeletionPhase::Replacing:
        return QByteArrayLiteral("replacing");
    case CredentialDeletionPhase::Creating:
        return QByteArrayLiteral("creating");
    case CredentialDeletionPhase::Updating:
        return QByteArrayLiteral("updating");
    case CredentialDeletionPhase::Active:
        return QByteArrayLiteral("active");
    case CredentialDeletionPhase::Absent:
        return {};
    }
    return {};
}

CredentialDeletionPhase credentialDeletionPhase(
    const QByteArray &name,
    bool *valid)
{
    if (valid) *valid = true;
    if (name == QByteArrayLiteral("preparing-creation"))
        return CredentialDeletionPhase::PreparingCreation;
    if (name == QByteArrayLiteral("preparing"))
        return CredentialDeletionPhase::Preparing;
    if (name == QByteArrayLiteral("preparing-replacement"))
        return CredentialDeletionPhase::PreparingReplacement;
    if (name == QByteArrayLiteral("deleting"))
        return CredentialDeletionPhase::Deleting;
    if (name == QByteArrayLiteral("deleted"))
        return CredentialDeletionPhase::Deleted;
    if (name == QByteArrayLiteral("replacing"))
        return CredentialDeletionPhase::Replacing;
    if (name == QByteArrayLiteral("creating"))
        return CredentialDeletionPhase::Creating;
    if (name == QByteArrayLiteral("updating"))
        return CredentialDeletionPhase::Updating;
    if (name == QByteArrayLiteral("active"))
        return CredentialDeletionPhase::Active;
    if (valid) *valid = false;
    return CredentialDeletionPhase::Absent;
}

QByteArray credentialDeletionContents(
    CredentialDeletionPhase phase,
    const QByteArray &transaction)
{
    const QByteArray phaseName =
        credentialDeletionPhaseName(phase);
    if (phaseName.isEmpty()
        || !isCredentialTransaction(transaction)) {
        return {};
    }
    return QByteArrayLiteral("v1 ") + phaseName
        + QByteArrayLiteral(" ") + transaction
        + QByteArrayLiteral("\n");
}

bool credentialDurabilityFailure(
    const QByteArray &stage)
{
#ifdef GC_CREDENTIAL_STORE_CUSTOM_FACTORY
    static QMutex mutex;
    static QByteArray configuredFailure;
    static QHash<QByteArray, int> observations;
    QMutexLocker locker(&mutex);
    const QByteArray requested = qgetenv(
        "GC_CREDENTIAL_TEST_DURABILITY_FAILURE");
    if (requested != configuredFailure) {
        configuredFailure = requested;
        observations.clear();
    }
    if (requested == stage)
        return true;
    const QList<QByteArray> fields = requested.split(':');
    bool validOrdinal = false;
    const int ordinal = fields.size() == 2
        ? fields.at(1).toInt(&validOrdinal)
        : 0;
    if (fields.size() != 2
        || fields.at(0) != stage
        || !validOrdinal
        || ordinal <= 0) {
        return false;
    }
    const int observed = ++observations[stage];
    return observed == ordinal;
#else
    Q_UNUSED(stage);
    return false;
#endif
}

void credentialCrashPoint(const QByteArray &point)
{
#ifdef GC_CREDENTIAL_STORE_CUSTOM_FACTORY
    if (qgetenv("GC_CREDENTIAL_TEST_CRASH_POINT")
        == point) {
        std::_Exit(86);
    }
#else
    Q_UNUSED(point);
#endif
}

#ifdef Q_OS_UNIX
bool syncCredentialDescriptor(int descriptor)
{
#ifdef Q_OS_DARWIN
    int fullSyncResult;
    do {
        fullSyncResult =
            ::fcntl(descriptor, F_FULLFSYNC);
    } while (fullSyncResult == -1
             && errno == EINTR);
    if (fullSyncResult == 0)
        return true;
    if (errno != EINVAL && errno != ENOTSUP)
        return false;
#endif

    int result;
    do {
        result = ::fsync(descriptor);
    } while (result == -1 && errno == EINTR);
    return result == 0;
}

int openCredentialPath(
    const QString &path,
    int accessFlags)
{
    int flags = accessFlags;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor;
    const QByteArray encoded = QFile::encodeName(path);
    do {
        descriptor = ::open(encoded.constData(), flags);
    } while (descriptor == -1 && errno == EINTR);
    return descriptor;
}

bool syncCredentialDirectoryPath(
    const QString &directoryPath)
{
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor =
        openCredentialPath(directoryPath, flags);
    if (descriptor == -1)
        return false;
    const bool synchronized =
        syncCredentialDescriptor(descriptor);
    return ::close(descriptor) == 0 && synchronized;
}
#endif

bool credentialDirectoryTreeIsDurable(
    const QString &directoryPath,
    const QString &existingAncestorPath,
    bool force)
{
    static QMutex durabilityMutex;
    static QSet<QString> durableDirectories;
    QMutexLocker locker(&durabilityMutex);
    if (!force
        && durableDirectories.contains(
            directoryPath)) {
        return true;
    }
    if (credentialDurabilityFailure(
            QByteArrayLiteral("ancestor"))) {
        return false;
    }
#ifdef Q_OS_UNIX
    QString current =
        QDir::cleanPath(directoryPath);
    const QString existingAncestor =
        QDir::cleanPath(existingAncestorPath);
    const QString relative =
        QDir(existingAncestor).relativeFilePath(current);
    if (relative == QStringLiteral("..")
        || relative.startsWith(
            QStringLiteral("../"))
        || QDir::isAbsolutePath(relative)) {
        return false;
    }

    while (true) {
        if (!syncCredentialDirectoryPath(current))
            return false;
        if (current == existingAncestor) {
            durableDirectories.insert(directoryPath);
            return true;
        }
        const QString parent =
            QFileInfo(current).absolutePath();
        if (parent == current)
            return false;
        current = parent;
    }
#else
    Q_UNUSED(existingAncestorPath);
    durableDirectories.insert(directoryPath);
    return true;
#endif
}

bool syncCredentialFile(const QString &path)
{
    if (credentialDurabilityFailure(
            QByteArrayLiteral("file"))) {
        return false;
    }
#ifdef Q_OS_UNIX
    const int descriptor =
        openCredentialPath(path, O_RDWR);
    if (descriptor == -1)
        return false;
    const bool synchronized =
        syncCredentialDescriptor(descriptor);
    return ::close(descriptor) == 0 && synchronized;
#elif defined(Q_OS_WIN)
    ScopedWindowsHandle file =
        openWindowsCredentialFile(
            path, GENERIC_READ | GENERIC_WRITE);
    if (!file.isValid()
        || !windowsHandleIsUnredirectedFile(
            file.get())) {
        return false;
    }
    const bool synchronized =
        ::FlushFileBuffers(file.get());
    const bool closed =
        ::CloseHandle(file.release());
    return synchronized && closed;
#else
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    const bool synchronized = file.flush();
    file.close();
    return synchronized
        && file.error() == QFileDevice::NoError;
#endif
}

bool syncCredentialDirectory(const QString &path)
{
    if (credentialDurabilityFailure(
            QByteArrayLiteral("directory"))) {
        return false;
    }
#ifdef Q_OS_UNIX
    return syncCredentialDirectoryPath(
        QFileInfo(path).absolutePath());
#else
    Q_UNUSED(path);
    return true;
#endif
}

bool credentialFileIsDurable(const QString &path)
{
    return syncCredentialFile(path)
        && syncCredentialDirectory(path);
}

bool replaceCredentialFile(
    const QString &path,
    const QByteArray &contents)
{
    ReplaceAtomicFileWriter file(path);
    if (!file.open())
        return false;
    if (file.write(contents) != contents.size()
        || !file.flush()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool readCredentialFile(
    const QString &path,
    qint64 maximumSize,
    QByteArray *contents)
{
    if (!contents || maximumSize < 0)
        return false;
#ifdef Q_OS_UNIX
    const int descriptor =
        openCredentialPath(path, O_RDONLY);
    if (descriptor == -1)
        return false;
    struct stat information;
    if (::fstat(descriptor, &information) != 0
        || !S_ISREG(information.st_mode)) {
        ::close(descriptor);
        return false;
    }
    QFile file;
    if (!file.open(
            descriptor, QIODevice::ReadOnly,
            QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return false;
    }
#elif defined(Q_OS_WIN)
    ScopedWindowsHandle nativeFile =
        openWindowsCredentialFile(path, GENERIC_READ);
    if (!nativeFile.isValid()
        || !windowsHandleIsUnredirectedFile(
            nativeFile.get())) {
        return false;
    }
    HANDLE rawHandle = nativeFile.release();
    const int descriptor = ::_open_osfhandle(
        reinterpret_cast<intptr_t>(rawHandle),
        _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        ::CloseHandle(rawHandle);
        return false;
    }
    QFile file;
    if (!file.open(
            descriptor, QIODevice::ReadOnly,
            QFileDevice::AutoCloseHandle)) {
        ::_close(descriptor);
        return false;
    }
#else
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
#endif
    const QByteArray read = file.read(maximumSize);
    if (!file.atEnd()
        || file.error() != QFileDevice::NoError) {
        return false;
    }
    *contents = read;
    return true;
}

CredentialDeletionState readCredentialDeletionState(
    const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo information(path);
    if (credentialPathIsRedirected(information))
        return {};
    if (!information.exists()) {
        return {true, CredentialDeletionPhase::Absent,
                QByteArray()};
    }
    if (!information.isFile())
        return {};

    QByteArray contents;
    if (!readCredentialFile(path, 64, &contents))
        return {};
    const QList<QByteArray> fields =
        contents.trimmed().split(' ');
    if (fields.size() != 3
        || fields.at(0) != QByteArrayLiteral("v1")
        || !isCredentialTransaction(fields.at(2))) {
        return {};
    }
    bool validPhase = false;
    const CredentialDeletionPhase phase =
        credentialDeletionPhase(fields.at(1), &validPhase);
    if (!validPhase
        || contents
            != credentialDeletionContents(
                phase, fields.at(2))) {
        return {};
    }
    return {true, phase, fields.at(2)};
}

bool writeCredentialDeletionState(
    const QString &path,
    CredentialDeletionPhase phase,
    const QByteArray &transaction)
{
    if (path.isEmpty()
        || phase == CredentialDeletionPhase::Absent
        || !isCredentialTransaction(transaction)) {
        return false;
    }
    const QFileInfo existing(path);
    if (credentialPathIsRedirected(existing)
        || (existing.exists() && !existing.isFile())) {
        return false;
    }
    const QByteArray contents =
        credentialDeletionContents(phase, transaction);
    if (!replaceCredentialFile(path, contents))
        return false;
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(
            path,
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner)) {
        return false;
    }
#endif
    if (!credentialFileIsDurable(path))
        return false;
    const CredentialDeletionState persisted =
        readCredentialDeletionState(path);
    const bool committed = persisted.readable
        && persisted.phase == phase
        && persisted.transaction == transaction;
    if (committed) {
        credentialCrashPoint(
            QByteArrayLiteral("deletion:")
            + credentialDeletionPhaseName(phase));
    }
    return committed;
}

CredentialRevision readCredentialRevision(
    const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo information(path);
    if (!information.exists())
        return {true, QByteArray()};
    if (!information.isFile()
        || credentialPathIsRedirected(information))
        return {};

    QByteArray contents;
    if (!readCredentialFile(path, 34, &contents))
        return {};
    const QByteArray revision = contents.trimmed();
    if (!isCredentialTransaction(revision))
        return {};
    return {true, revision};
}

bool advanceCredentialRevision(
    const QString &path,
    QByteArray *revision)
{
    if (path.isEmpty())
        return false;
    const QFileInfo existing(path);
    if (existing.exists()
        && (!existing.isFile()
            || credentialPathIsRedirected(existing))) {
        return false;
    }
    const QByteArray next =
        QUuid::createUuid().toRfc4122().toHex();
    const QByteArray contents =
        next + QByteArrayLiteral("\n");
    if (!replaceCredentialFile(path, contents))
        return false;
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(
            path,
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner)) {
        return false;
    }
#endif
    if (!credentialFileIsDurable(path))
        return false;
    const CredentialRevision persisted =
        readCredentialRevision(path);
    if (!persisted.readable
        || persisted.value != next) {
        return false;
    }
    if (revision) *revision = next;
    return true;
}

CredentialRevision currentCredentialRevision(
    const QString &path)
{
    CredentialRevision current =
        readCredentialRevision(path);
    if (current.readable && !current.value.isEmpty())
        return current;

    QByteArray baseline;
    if (!advanceCredentialRevision(path, &baseline))
        return {};
    return {true, baseline};
}

bool finalizeCredentialWrite(
    const QString &revisionPath,
    const QString &deletionPath,
    const QByteArray &transaction,
    QByteArray *revision)
{
    QByteArray finalRevision;
    if (!advanceCredentialRevision(
            revisionPath, &finalRevision)) {
        return false;
    }
    credentialCrashPoint(
        QByteArrayLiteral("write:final-revision"));
    if (!writeCredentialDeletionState(
            deletionPath,
            CredentialDeletionPhase::Active,
            transaction)) {
        return false;
    }
    if (revision) *revision = finalRevision;
    return true;
}

std::unique_ptr<QSettings> freshExactSettings(
    QSettings *settings)
{
    if (!settings || settings->fileName().isEmpty())
        return {};
    auto exact = std::make_unique<QSettings>(
        settings->fileName(), settings->format());
    exact->setFallbacksEnabled(false);
    exact->setAtomicSyncRequired(true);
    const QString group = settings->group();
    if (!group.isEmpty())
        exact->beginGroup(group);
    return exact;
}

struct ExactSetting
{
    bool readable = false;
    bool present = false;
    QVariant value;
};

ExactSetting readExactSetting(
    QSettings *settings,
    const QString &key)
{
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return {};
    exact->sync();
    if (exact->status() != QSettings::NoError)
        return {};
    const bool present = exact->contains(key);
    return {true, present,
            present ? exact->value(key) : QVariant()};
}

bool writeExactSetting(
    QSettings *settings,
    const QString &key,
    const QVariant &value)
{
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->setValue(key, value);
    exact->sync();
    CredentialSettings::hardenSettingsFile(settings);
    return exact->status() == QSettings::NoError
        && exact->contains(key)
        && exact->value(key) == value;
}

bool removeExactSetting(
    QSettings *settings,
    const QString &key)
{
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->remove(key);
    exact->sync();
    CredentialSettings::hardenSettingsFile(settings);
    return exact->status() == QSettings::NoError
        && !exact->contains(key);
}

QString pendingRemovalGenerationKey(
    const QString &removalKey)
{
    const QString prefix =
        QStringLiteral("credential_store/pending_remove/");
    if (!removalKey.startsWith(prefix))
        return {};
    return QStringLiteral(
        "credential_store/pending_remove_generation/")
        + removalKey.mid(prefix.size());
}

struct PendingRemoval
{
    bool readable = false;
    bool metadataPresent = false;
    bool requested = false;
    bool generationPresent = false;
    bool generationValid = false;
    QByteArray generation;
};

PendingRemoval readPendingRemoval(
    QSettings *settings,
    const QString &removalKey)
{
    const QString generationKey =
        pendingRemovalGenerationKey(removalKey);
    if (generationKey.isEmpty())
        return {};
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return {};
    exact->sync();
    if (exact->status() != QSettings::NoError)
        return {};

    const bool markerPresent =
        exact->contains(removalKey);
    const bool generationPresent =
        exact->contains(generationKey);
    const QByteArray generation = generationPresent
        ? exact->value(generationKey)
              .toString().toLatin1()
        : QByteArray();
    return {
        true,
        markerPresent || generationPresent,
        markerPresent
            && exact->value(removalKey).toBool(),
        generationPresent,
        generationPresent
            && isCredentialTransaction(generation),
        generation
    };
}

bool writePendingRemoval(
    QSettings *settings,
    const QString &removalKey,
    const QByteArray &generation)
{
    const QString generationKey =
        pendingRemovalGenerationKey(removalKey);
    if (generationKey.isEmpty()
        || !isCredentialTransaction(generation)) {
        return false;
    }
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->setValue(removalKey, true);
    exact->setValue(
        generationKey,
        QString::fromLatin1(generation));
    exact->sync();
    CredentialSettings::hardenSettingsFile(settings);
    const bool persisted =
        exact->status() == QSettings::NoError
        && exact->value(removalKey).toBool()
        && exact->value(generationKey)
               .toString().toLatin1()
            == generation;
#ifdef GC_CREDENTIAL_STORE_CUSTOM_FACTORY
    if (persisted
        && qEnvironmentVariableIsSet(
            "GC_CREDENTIAL_TEST_PENDING_RESULT_FAILURE")) {
        return false;
    }
#endif
    return persisted;
}

bool removePendingRemoval(
    QSettings *settings,
    const QString &removalKey)
{
    const QString generationKey =
        pendingRemovalGenerationKey(removalKey);
    if (generationKey.isEmpty())
        return false;
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->remove(removalKey);
    exact->remove(generationKey);
    exact->sync();
    CredentialSettings::hardenSettingsFile(settings);
    return exact->status() == QSettings::NoError
        && !exact->contains(removalKey)
        && !exact->contains(generationKey);
}

enum class PendingRemovalDisposition
{
    None,
    Current,
    Stale,
    Invalid
};

PendingRemovalDisposition pendingRemovalDisposition(
    const PendingRemoval &pending,
    const CredentialDeletionState &deletion)
{
    if (!pending.readable)
        return PendingRemovalDisposition::Invalid;
    if (!pending.requested) {
        if (pending.generationPresent
            && !pending.generationValid
            && deletion.phase
                == CredentialDeletionPhase::Absent) {
            return PendingRemovalDisposition::Invalid;
        }
        return PendingRemovalDisposition::None;
    }

    if (deletion.phase
            == CredentialDeletionPhase::Deleting
        || deletion.phase
            == CredentialDeletionPhase::Deleted) {
        return PendingRemovalDisposition::Current;
    }
    if (supersedesPendingRemoval(deletion.phase))
        return PendingRemovalDisposition::Stale;
    if (isCredentialDeletionPreparation(
            deletion.phase)) {
        return pending.generationValid
                && pending.generation
                    == deletion.transaction
            ? PendingRemovalDisposition::Current
            : PendingRemovalDisposition::Stale;
    }
    if (deletion.phase
        == CredentialDeletionPhase::Absent) {
        return !pending.generationPresent
            ? PendingRemovalDisposition::Current
            : PendingRemovalDisposition::Invalid;
    }
    return PendingRemovalDisposition::Invalid;
}

const QSet<QString> &credentialKeys()
{
    static const QSet<QString> keys = {
        QStringLiteral(GC_RWGPSPASS),
        QStringLiteral(GC_RWGPS_AUTH_TOKEN),
        QStringLiteral(GC_TTBPASS),
        QStringLiteral(GC_SPORTPLUSHEALTHPASS),
        QStringLiteral(GC_SELPASS),
        QStringLiteral(GC_WIKEY),
        QStringLiteral(GC_DVPASS),
        QStringLiteral(GC_DROPBOX_TOKEN),
        QStringLiteral(GC_WITHINGS_TOKEN),
        QStringLiteral(GC_WITHINGS_SECRET),
        QStringLiteral(GC_NOKIA_TOKEN),
        QStringLiteral(GC_NOKIA_REFRESH_TOKEN),
        QStringLiteral(GC_STRAVA_TOKEN),
        QStringLiteral(GC_STRAVA_REFRESH_TOKEN),
        QStringLiteral(GC_CYCLINGANALYTICS_TOKEN),
        QStringLiteral(GC_SIXCYCLE_PASS),
        QStringLiteral(GC_AZUM_ACCESS_TOKEN),
        QStringLiteral(GC_AZUM_REFRESH_TOKEN),
        QStringLiteral(GC_AZUM_USERKEY),
        QStringLiteral(GC_TREDICT_TOKEN),
        QStringLiteral(GC_TREDICT_REFRESH_TOKEN),
        QStringLiteral(GC_POLARFLOW_TOKEN),
        QStringLiteral(GC_SPORTTRACKS_TOKEN),
        QStringLiteral(GC_SPORTTRACKS_REFRESH_TOKEN),
        QStringLiteral(GC_XERTPASS),
        QStringLiteral(GC_XERT_TOKEN),
        QStringLiteral(GC_XERT_REFRESH_TOKEN),
        QStringLiteral(GC_NOLIO_ACCESS_TOKEN),
        QStringLiteral(GC_NOLIO_REFRESH_TOKEN)
    };
    return keys;
}

} // namespace

CredentialSettings::CredentialSettings(
    std::unique_ptr<CredentialStore> store)
    : store_(std::move(store))
{
}

bool CredentialSettings::isCredentialKey(const QString &key)
{
    return credentialKeys().contains(key);
}

QStringList CredentialSettings::credentialKeysForPrefix(
    const QString &prefix)
{
    QStringList result;
    for (const QString &key : credentialKeys()) {
        if (key.startsWith(prefix)) result.append(key);
    }
    result.sort();
    return result;
}

QString CredentialSettings::ensureScopeId(
    QSettings *settings,
    const QString &storageKey,
    const QString &preferredScopeId)
{
    if (!settings || storageKey.isEmpty()) return QString();
    const QString settingsIdentity =
        canonicalSettingsFileName(settings);
    if (settingsIdentity.isEmpty()) return QString();
    const QString operationId =
        QStringLiteral("scope\n")
        + settingsIdentity + QLatin1Char('\n')
        + settings->group()
        + QLatin1Char('\n') + storageKey;
    CredentialOperationGuard operation(operationId);
    if (!operation)
        return QString();

    const ExactSetting storedSetting =
        readExactSetting(settings, storageKey);
    if (!storedSetting.readable)
        return QString();
    const QString stored = storedSetting.value.toString();
    const QUuid storedId(stored);
    if (!storedId.isNull()) {
        const QString scopeId =
            storedId.toString(QUuid::WithoutBraces);
        if (stored != scopeId
            && !writeExactSetting(
                settings, storageKey, scopeId)) {
            return QString();
        }
        return scopeId;
    }

    const QUuid preferredId(preferredScopeId);
    const QString scopeId = preferredId.isNull()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : preferredId.toString(QUuid::WithoutBraces);
    if (!writeExactSetting(settings, storageKey, scopeId)) {
        qWarning() << "Cannot persist credential store scope:"
                   << settings->fileName();
        return QString();
    }
    return scopeId;
}

QString CredentialSettings::vaultKey(
    const QString &scopeId,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        credentialKey.toUtf8(), QCryptographicHash::Sha256).toHex();
    return scopeId + QLatin1Char('/')
        + QString::fromLatin1(digest);
}

void CredentialSettings::hardenSettingsFile(QSettings *settings)
{
#ifdef Q_OS_UNIX
    if (!settings) return;
    const QString fileName = settings->fileName();
    if (fileName.isEmpty() || !QFileInfo::exists(fileName)) return;
    if (!QFile::setPermissions(
            fileName,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        qWarning() << "Cannot restrict settings file permissions:"
                   << fileName;
    }
#else
    Q_UNUSED(settings)
#endif
}

QVariant CredentialSettings::value(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QVariant &defaultValue)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return defaultValue;
    }

    const QString key = vaultKey(scopeId, credentialKey);
    CredentialOperationGuard operation(key);
    if (!operation)
        return defaultValue;
    CredentialDeletionState deletion =
        readCredentialDeletionState(operation.deletionPath());
    if (!deletion.readable) {
        invalidateCache(key);
        return defaultValue;
    }
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    const PendingRemoval pending =
        readPendingRemoval(settings, removalKey);
    const PendingRemovalDisposition disposition =
        pendingRemovalDisposition(pending, deletion);
    if (disposition
        == PendingRemovalDisposition::Invalid) {
        return defaultValue;
    }
    if (disposition
        == PendingRemovalDisposition::Current) {
        completePendingRemoval(
            settings, key, credentialKey, plaintextKey,
            removalKey, operation.revisionPath(),
            operation.deletionPath());
        return defaultValue;
    }
    if ((disposition
             == PendingRemovalDisposition::Stale
         || pending.metadataPresent)
        && !removePendingRemoval(settings, removalKey)) {
        invalidateCache(key);
        return defaultValue;
    }

    if (isCredentialDeletionPreparation(
            deletion.phase)) {
        invalidateCache(key);
        return defaultValue;
    }
    if (deletion.phase
        == CredentialDeletionPhase::Deleting) {
        completePendingRemoval(
            settings, key, credentialKey, plaintextKey,
            removalKey, operation.revisionPath(),
            operation.deletionPath());
        return defaultValue;
    }
    if (deletion.phase
        == CredentialDeletionPhase::Deleted) {
        completePendingRemoval(
            settings, key, credentialKey, plaintextKey,
            removalKey, operation.revisionPath(),
            operation.deletionPath());
        return defaultValue;
    }

    const ExactSetting plaintext =
        readExactSetting(settings, plaintextKey);
    if (!plaintext.readable)
        return defaultValue;

    const CredentialRevision revision =
        currentCredentialRevision(operation.revisionPath());
    CacheEntry entry;
    bool haveCached =
        revision.readable && cached(key, &entry);
    if (haveCached && entry.revision != revision.value) {
        invalidateCache(key);
        haveCached = false;
    } else if (haveCached
               && deletion.phase
                   != CredentialDeletionPhase::Absent
               && entry.transaction
                   != deletion.transaction) {
        invalidateCache(key);
        haveCached = false;
    } else if (!revision.readable) {
        invalidateCache(key);
    }

    if (deletion.phase
        == CredentialDeletionPhase::Creating) {
        if (haveCached && entry.present
            && !entry.persisted) {
            return entry.value;
        }

        const CredentialStore::ReadResult result = store_
            ? store_->read(key)
            : CredentialStore::ReadResult{
                  CredentialStore::Status::Unavailable,
                  QString(),
                  QStringLiteral("No credential store")};
        if (result.status
            == CredentialStore::Status::Success) {
            const bool scrubbed = !plaintext.present
                || scrubPlaintext(settings, plaintextKey);
            QByteArray finalRevision;
            if (scrubbed
                && finalizeCredentialWrite(
                    operation.revisionPath(),
                    operation.deletionPath(),
                    deletion.transaction,
                    &finalRevision)) {
                entry = {true, result.value, true,
                         finalRevision,
                         deletion.transaction};
                cache(key, entry);
            } else {
                invalidateCache(key);
            }
            return result.value;
        }
        invalidateCache(key);
        if (result.status
            != CredentialStore::Status::NotFound) {
            reportStoreError(
                QStringLiteral("read"), credentialKey,
                result.error);
            return defaultValue;
        }
        if (!plaintext.present) {
            return defaultValue;
        }

        const QString legacy =
            plaintext.value.toString();
        if (legacy.isEmpty()) {
            scrubPlaintext(settings, plaintextKey);
            return defaultValue;
        }

        QByteArray migrationRevision;
        if (!advanceCredentialRevision(
                operation.revisionPath(),
                &migrationRevision)) {
            reportStoreError(
                QStringLiteral("migrate"), credentialKey,
                QStringLiteral(
                    "Cannot persist credential revision"));
            hardenSettingsFile(settings);
            return legacy;
        }
        QString error;
        const CredentialStore::Status writeStatus = store_
            ? store_->write(key, legacy, &error)
            : CredentialStore::Status::Unavailable;
        if (writeStatus
            != CredentialStore::Status::Success) {
            reportStoreError(
                QStringLiteral("migrate"), credentialKey,
                error);
            cache(key, {true, legacy, false,
                        migrationRevision,
                        deletion.transaction});
            hardenSettingsFile(settings);
            return legacy;
        }
        credentialCrashPoint(
            QByteArrayLiteral("vault:written"));
        if (!scrubPlaintext(settings, plaintextKey)) {
            invalidateCache(key);
            return legacy;
        }
        QByteArray finalRevision;
        if (!finalizeCredentialWrite(
                operation.revisionPath(),
                operation.deletionPath(),
                deletion.transaction,
                &finalRevision)) {
            invalidateCache(key);
            return legacy;
        }
        cache(key, {true, legacy, true,
                    finalRevision,
                    deletion.transaction});
        return legacy;
    }

    if (deletion.phase
        == CredentialDeletionPhase::Replacing) {
        const bool scrubbed = !plaintext.present
            || scrubPlaintext(settings, plaintextKey);
        if (!scrubbed) {
            invalidateCache(key);
            return defaultValue;
        }
        if (haveCached && entry.present
            && !entry.persisted) {
            return entry.value;
        }

        const CredentialStore::ReadResult result = store_
            ? store_->read(key)
            : CredentialStore::ReadResult{
                  CredentialStore::Status::Unavailable,
                  QString(),
                  QStringLiteral("No credential store")};
        if (result.status
            == CredentialStore::Status::Success) {
            QByteArray finalRevision;
            if (finalizeCredentialWrite(
                    operation.revisionPath(),
                    operation.deletionPath(),
                    deletion.transaction,
                    &finalRevision)) {
                entry = {true, result.value, true,
                         finalRevision,
                         deletion.transaction};
                cache(key, entry);
            } else {
                invalidateCache(key);
            }
            return result.value;
        }
        invalidateCache(key);
        if (result.status
            != CredentialStore::Status::NotFound) {
            reportStoreError(
                QStringLiteral("read"), credentialKey,
                result.error);
        }
        return defaultValue;
    }

    if (deletion.phase
        == CredentialDeletionPhase::Updating) {
        if (haveCached && entry.present
            && !entry.persisted) {
            return entry.value;
        }

        const CredentialStore::ReadResult result = store_
            ? store_->read(key)
            : CredentialStore::ReadResult{
                  CredentialStore::Status::Unavailable,
                  QString(),
                  QStringLiteral("No credential store")};
        if (result.status
            == CredentialStore::Status::Success) {
            const bool scrubbed = !plaintext.present
                || scrubPlaintext(settings, plaintextKey);
            QByteArray finalRevision;
            if (scrubbed
                && finalizeCredentialWrite(
                    operation.revisionPath(),
                    operation.deletionPath(),
                    deletion.transaction,
                    &finalRevision)) {
                entry = {true, result.value, true,
                         finalRevision,
                         deletion.transaction};
                cache(key, entry);
            } else {
                invalidateCache(key);
            }
            return result.value;
        }
        if (result.status
            != CredentialStore::Status::NotFound) {
            invalidateCache(key);
            reportStoreError(
                QStringLiteral("read"), credentialKey,
                result.error);
            return defaultValue;
        }

        const bool scrubbed = !plaintext.present
            || scrubPlaintext(settings, plaintextKey);
        QByteArray finalRevision;
        if (scrubbed
            && finalizeCredentialWrite(
                operation.revisionPath(),
                operation.deletionPath(),
                deletion.transaction,
                &finalRevision)) {
            cache(key, {false, QString(), false,
                        finalRevision,
                        deletion.transaction});
        } else {
            invalidateCache(key);
        }
        return defaultValue;
    }

    if (deletion.phase
        == CredentialDeletionPhase::Active) {
        const bool scrubbed = !plaintext.present
            || scrubPlaintext(settings, plaintextKey);
        if (!scrubbed) {
            invalidateCache(key);
            return defaultValue;
        }
        if (haveCached && entry.present
            && !entry.persisted) {
            invalidateCache(key);
            haveCached = false;
        }
        if (haveCached) {
            return entry.present
                ? QVariant(entry.value) : defaultValue;
        }

        const CredentialStore::ReadResult result = store_
            ? store_->read(key)
            : CredentialStore::ReadResult{
                  CredentialStore::Status::Unavailable,
                  QString(),
                  QStringLiteral("No credential store")};
        if (result.status
            == CredentialStore::Status::Success) {
            entry = {true, result.value, true,
                     revision.value,
                     deletion.transaction};
            if (revision.readable) {
                cache(key, entry);
            } else {
                invalidateCache(key);
            }
            return result.value;
        }
        invalidateCache(key);
        if (result.status
            == CredentialStore::Status::NotFound) {
            if (revision.readable) {
                cache(key, {false, QString(), false,
                            revision.value,
                            deletion.transaction});
            }
        } else {
            reportStoreError(
                QStringLiteral("read"), credentialKey,
                result.error);
        }
        return defaultValue;
    }

    if (haveCached) {
        if (entry.persisted && plaintext.present) {
            scrubPlaintext(settings, plaintextKey);
        }
        if (entry.present || !plaintext.present) {
            return entry.present ? QVariant(entry.value) : defaultValue;
        }
    }

    const CredentialStore::ReadResult result = store_
        ? store_->read(key)
        : CredentialStore::ReadResult{
              CredentialStore::Status::Unavailable,
              QString(), QStringLiteral("No credential store")};
    if (result.status == CredentialStore::Status::Success) {
        entry = {true, result.value, true,
                 revision.value, QByteArray()};
        const bool scrubbed =
            scrubPlaintext(settings, plaintextKey);
        if (revision.readable && scrubbed) {
            cache(key, entry);
        } else {
            invalidateCache(key);
        }
        return entry.value;
    }

    if (plaintext.present) {
        const QString legacy = plaintext.value.toString();
        if (legacy.isEmpty()) {
            const bool scrubbed =
                scrubPlaintext(settings, plaintextKey);
            if (result.status
                == CredentialStore::Status::NotFound) {
                if (scrubbed && revision.readable) {
                    entry.revision = revision.value;
                    cache(key, entry);
                } else {
                    invalidateCache(key);
                }
            } else {
                invalidateCache(key);
                reportStoreError(
                    QStringLiteral("read"), credentialKey,
                    result.error);
            }
            return defaultValue;
        }

        // Only an authoritative miss permits a migration write. A transient
        // read failure may be hiding a newer credential already in the vault.
        if (result.status != CredentialStore::Status::NotFound) {
            reportStoreError(
                QStringLiteral("read"), credentialKey,
                result.error);
            hardenSettingsFile(settings);
            return defaultValue;
        }

        QByteArray migrationRevision;
        if (!advanceCredentialRevision(
                operation.revisionPath(),
                &migrationRevision)) {
            invalidateCache(key);
            reportStoreError(
                QStringLiteral("migrate"), credentialKey,
                QStringLiteral(
                    "Cannot persist credential revision"));
            hardenSettingsFile(settings);
            return legacy;
        }

        QString error;
        const CredentialStore::Status writeStatus = store_
            ? store_->write(key, legacy, &error)
            : CredentialStore::Status::Unavailable;
        const bool migrated =
            writeStatus == CredentialStore::Status::Success;
        if (!migrated) {
            reportStoreError(
                QStringLiteral("migrate"), credentialKey,
                error.isEmpty() ? result.error : error);
            hardenSettingsFile(settings);
        }
        entry = {true, legacy, migrated,
                 migrationRevision, QByteArray()};
        if (!migrated) {
            cache(key, entry);
        } else if (scrubPlaintext(settings, plaintextKey)) {
            cache(key, entry);
        } else {
            invalidateCache(key);
        }
        return entry.value;
    }

    if (result.status != CredentialStore::Status::NotFound) {
        reportStoreError(
            QStringLiteral("read"), credentialKey, result.error);
    } else if (revision.readable) {
        entry.revision = revision.value;
        cache(key, entry);
    }
    return defaultValue;
}

void CredentialSettings::setValue(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QVariant &value)
{
    setValueChecked(
        settings, scopeId, credentialKey, plaintextKey, value);
}

bool CredentialSettings::setValueChecked(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QVariant &value)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return false;
    }

    const QString key = vaultKey(scopeId, credentialKey);
    CredentialOperationGuard operation(key);
    if (!operation)
        return false;
    CredentialDeletionState deletion =
        readCredentialDeletionState(operation.deletionPath());
    if (!deletion.readable) {
        invalidateCache(key);
        return false;
    }
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    const QString secret = value.toString();
    if (secret.isEmpty()) {
        if (deletion.phase
                == CredentialDeletionPhase::Deleting
            || deletion.phase
                == CredentialDeletionPhase::Deleted) {
            return completePendingRemoval(
                settings, key, credentialKey,
                plaintextKey, removalKey,
                operation.revisionPath(),
                operation.deletionPath());
        }

        QByteArray transaction =
            deletion.transaction;
        if (!isCredentialDeletionPreparation(
                deletion.phase)) {
            transaction = newCredentialTransaction();
            if (!writeCredentialDeletionState(
                    operation.deletionPath(),
                    credentialDeletionPreparationFor(
                        deletion.phase),
                    transaction)) {
                invalidateCache(key);
                return false;
            }
        }
        if (!persistPendingRemoval(
                settings, removalKey, transaction)) {
            invalidateCache(key);
            return false;
        }
        return completePendingRemoval(
            settings, key, credentialKey,
            plaintextKey, removalKey,
            operation.revisionPath(),
            operation.deletionPath());
    }

    const PendingRemoval pending =
        readPendingRemoval(settings, removalKey);
    const PendingRemovalDisposition disposition =
        pendingRemovalDisposition(pending, deletion);
    if (disposition
        == PendingRemovalDisposition::Invalid) {
        return false;
    }
    if (disposition
        == PendingRemovalDisposition::Current) {
        if (!completePendingRemoval(
                settings, key, credentialKey,
                plaintextKey, removalKey,
                operation.revisionPath(),
                operation.deletionPath())) {
            return false;
        }
        deletion = readCredentialDeletionState(
            operation.deletionPath());
        if (!deletion.readable)
            return false;
    } else if ((disposition
                    == PendingRemovalDisposition::Stale
                || pending.metadataPresent)
               && !removePendingRemoval(
                   settings, removalKey)) {
        invalidateCache(key);
        return false;
    }

    if (deletion.phase
        == CredentialDeletionPhase::Deleting) {
        if (!completePendingRemoval(
                settings, key, credentialKey,
                plaintextKey, removalKey,
                operation.revisionPath(),
                operation.deletionPath())) {
            return false;
        }
        deletion = readCredentialDeletionState(
            operation.deletionPath());
        if (!deletion.readable)
            return false;
    }

    const CredentialDeletionPhase writePhase =
        credentialWritePhaseFor(deletion.phase);
    const QByteArray transaction =
        newCredentialTransaction();
    if (!writeCredentialDeletionState(
            operation.deletionPath(), writePhase,
            transaction)) {
        invalidateCache(key);
        return false;
    }

    QByteArray revision;
    if (!advanceCredentialRevision(
            operation.revisionPath(), &revision)) {
        invalidateCache(key);
        reportStoreError(
            QStringLiteral("write"), credentialKey,
            QStringLiteral(
                "Cannot persist credential revision"));
        return false;
    }

    QString error;
    const CredentialStore::Status status = store_
        ? store_->write(key, secret, &error)
        : CredentialStore::Status::Unavailable;
    const bool persisted = status == CredentialStore::Status::Success;
    bool scrubbed = false;
    if (persisted) {
        credentialCrashPoint(
            QByteArrayLiteral("vault:written"));
        scrubbed = scrubPlaintext(settings, plaintextKey);
    } else {
        reportStoreError(
            QStringLiteral("write"), credentialKey, error);
        hardenSettingsFile(settings);
    }
    if (!persisted) {
        cache(key, {true, secret, false, revision,
                    transaction});
        return false;
    }
    if (!scrubbed) {
        invalidateCache(key);
        return false;
    }

    QByteArray finalRevision;
    if (!finalizeCredentialWrite(
            operation.revisionPath(),
            operation.deletionPath(),
            transaction, &finalRevision)) {
        invalidateCache(key);
        return false;
    }
    cache(key, {true, secret, true, finalRevision,
                transaction});
    return true;
}

void CredentialSettings::remove(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey)
{
    removeChecked(
        settings, scopeId, credentialKey, plaintextKey);
}

bool CredentialSettings::persistPendingRemoval(
    QSettings *settings,
    const QString &removalKey,
    const QByteArray &generation)
{
    if (writePendingRemoval(
            settings, removalKey, generation)) {
        credentialCrashPoint(
            QByteArrayLiteral("pending-removal"));
        return true;
    }

    qWarning() << "Cannot persist pending credential removal:"
               << settings->fileName();
    return false;
}

bool CredentialSettings::completePendingRemoval(
    QSettings *settings,
    const QString &key,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QString &removalKey,
    const QString &revisionPath,
    const QString &deletionPath)
{
    CredentialDeletionState deletion =
        readCredentialDeletionState(deletionPath);
    if (!deletion.readable
        || supersedesPendingRemoval(deletion.phase)) {
        invalidateCache(key);
        return false;
    }

    PendingRemoval pending;
    if (deletion.phase
            == CredentialDeletionPhase::Absent
        || isCredentialDeletionPreparation(
            deletion.phase)) {
        pending = readPendingRemoval(settings, removalKey);
        if (pendingRemovalDisposition(
                pending, deletion)
            != PendingRemovalDisposition::Current) {
            invalidateCache(key);
            return false;
        }

        const QByteArray transaction =
            isCredentialDeletionPreparation(
                deletion.phase)
                ? deletion.transaction
                : (pending.generationValid
                       ? pending.generation
                       : newCredentialTransaction());
        if (!advanceCredentialRevision(
                revisionPath, nullptr)) {
            invalidateCache(key);
            reportStoreError(
                QStringLiteral("remove"), credentialKey,
                QStringLiteral(
                    "Cannot persist credential revision"));
            return false;
        }
        if (!writeCredentialDeletionState(
                deletionPath,
                CredentialDeletionPhase::Deleting,
                transaction)) {
            invalidateCache(key);
            reportStoreError(
                QStringLiteral("remove"), credentialKey,
                QStringLiteral(
                    "Cannot persist credential deletion state"));
            return false;
        }
        deletion.phase =
            CredentialDeletionPhase::Deleting;
        deletion.transaction = transaction;
    }

    invalidateCache(key);
    CredentialRevision revision;
    if (deletion.phase
        == CredentialDeletionPhase::Deleting) {
        revision = readCredentialRevision(revisionPath);
        if (!revision.readable
            || revision.value.isEmpty()) {
            reportStoreError(
                QStringLiteral("remove"), credentialKey,
                QStringLiteral(
                    "Cannot read credential revision"));
            return false;
        }
        if (!scrubPlaintext(settings, plaintextKey))
            return false;

        QString error;
        const CredentialStore::Status status = store_
            ? store_->remove(key, &error)
            : CredentialStore::Status::Unavailable;
        if (status != CredentialStore::Status::Success
            && status != CredentialStore::Status::NotFound) {
            reportStoreError(
                QStringLiteral("remove"), credentialKey,
                error);
            hardenSettingsFile(settings);
            return false;
        }
        credentialCrashPoint(
            QByteArrayLiteral("vault:removed"));
        if (!writeCredentialDeletionState(
                deletionPath,
                CredentialDeletionPhase::Deleted,
                deletion.transaction)) {
            reportStoreError(
                QStringLiteral("remove"), credentialKey,
                QStringLiteral(
                    "Cannot commit credential deletion state"));
            return false;
        }
        deletion.phase =
            CredentialDeletionPhase::Deleted;
    } else if (deletion.phase
               == CredentialDeletionPhase::Deleted) {
        revision = readCredentialRevision(revisionPath);
        if (!revision.readable
            || revision.value.isEmpty()) {
            reportStoreError(
                QStringLiteral("remove"), credentialKey,
                QStringLiteral(
                    "Cannot read credential revision"));
            return false;
        }
        if (!scrubPlaintext(settings, plaintextKey))
            return false;
    } else {
        return false;
    }

    pending = readPendingRemoval(settings, removalKey);
    if (!pending.readable) {
        return false;
    }
    if (pending.metadataPresent
        && !removePendingRemoval(settings, removalKey)) {
        return false;
    }

    cache(key, {false, QString(), false,
                revision.value,
                deletion.transaction});
    return true;
}

bool CredentialSettings::removeChecked(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey)
{
    return setValueChecked(
        settings, scopeId, credentialKey,
        plaintextKey, QVariant());
}

void CredentialSettings::migratePlaintext(
    QSettings *settings,
    const QString &scopeId,
    const QString &prefix)
{
    if (!settings || scopeId.isEmpty()) return;
    for (const QString &key : credentialKeysForPrefix(prefix)) {
        const QString storedKey = plaintextKey(key);
        const ExactSetting stored =
            readExactSetting(settings, storedKey);
        if (stored.readable && stored.present) {
            value(settings, scopeId, key, storedKey, QVariant());
        }
    }
}

void CredentialSettings::clearCache()
{
    QMutexLocker locker(&cacheMutex_);
    cache_.clear();
}

bool CredentialSettings::cached(
    const QString &key,
    CacheEntry *entry) const
{
    QMutexLocker locker(&cacheMutex_);
    const auto found = cache_.constFind(key);
    if (found == cache_.cend()) return false;
    if (entry) *entry = found.value();
    return true;
}

void CredentialSettings::cache(
    const QString &key,
    const CacheEntry &entry)
{
    QMutexLocker locker(&cacheMutex_);
    cache_.insert(key, entry);
}

void CredentialSettings::invalidateCache(const QString &key)
{
    QMutexLocker locker(&cacheMutex_);
    cache_.remove(key);
}

QString CredentialSettings::plaintextKey(
    const QString &credentialKey)
{
    const qsizetype marker = credentialKey.indexOf(QLatin1Char('>'));
    return marker >= 0 ? credentialKey.mid(marker + 1) : credentialKey;
}

QString CredentialSettings::pendingRemovalKey(
    const QString &scopeId,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        vaultKey(scopeId, credentialKey).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral("credential_store/pending_remove/")
        + QString::fromLatin1(digest);
}

bool CredentialSettings::scrubPlaintext(
    QSettings *settings,
    const QString &key)
{
    if (!settings || key.isEmpty()) return false;
    const ExactSetting stored =
        readExactSetting(settings, key);
    if (!stored.readable)
        return false;
    if (!stored.present)
        return true;

    if (removeExactSetting(settings, key))
        return true;
    qWarning() << "Cannot persist credential settings cleanup:"
               << settings->fileName();
    return false;
}

void CredentialSettings::reportStoreError(
    const QString &operation,
    const QString &credentialKey,
    const QString &error)
{
    qWarning() << "Credential store" << operation
               << "failed for" << credentialKey
               << (error.isEmpty()
                       ? QStringLiteral("unknown error")
                       : error)
               << "New plaintext credential writes are disabled.";
}
