#include <QtTest>

#include "AnchoredFileSystem.h"
#include "AtomicFileWriter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(Q_OS_LINUX)
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sys/xattr.h>
#endif
#if defined(Q_OS_MACOS)
#include <membership.h>
#include <sys/acl.h>
#include <uuid/uuid.h>
#endif
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <aclapi.h>
#endif

using namespace AnchoredFileSystem;

namespace {

using FilesystemAction = std::function<void(
    const char *, const QString &, const QString &)>;

FilesystemAction filesystemAction;
bool failDirectorySync = false;
bool failFileUnlink = false;
bool forceLegacyWindowsDelete = false;
bool forceZeroWindowsFileId = false;

#ifdef Q_OS_LINUX
struct LinuxAclXattr
{
    posix_acl_xattr_header header {};
    posix_acl_xattr_entry entries[5] {};
};

bool installLinuxAclXattr(
    const QString &path,
    const QByteArray &name,
    int &nativeError)
{
    LinuxAclXattr acl;
    acl.header.a_version = qToLittleEndian<quint32>(
        POSIX_ACL_XATTR_VERSION);
    const quint32 undefinedId = static_cast<quint32>(
        ACL_UNDEFINED_ID);
    const quint32 namedUser = ::geteuid() == 0 ? 1u : 0u;
    const auto setEntry = [&acl](
        int index, quint16 tag, quint16 permissions, quint32 id) {
        acl.entries[index].e_tag = qToLittleEndian<quint16>(tag);
        acl.entries[index].e_perm =
            qToLittleEndian<quint16>(permissions);
        acl.entries[index].e_id = qToLittleEndian<quint32>(id);
    };
    setEntry(0, ACL_USER_OBJ,
             ACL_READ | ACL_WRITE | ACL_EXECUTE, undefinedId);
    setEntry(1, ACL_USER, ACL_READ, namedUser);
    setEntry(2, ACL_GROUP_OBJ, 0, undefinedId);
    setEntry(3, ACL_MASK, ACL_READ, undefinedId);
    setEntry(4, ACL_OTHER, 0, undefinedId);

    const QByteArray encodedPath = QFile::encodeName(path);
    if (::setxattr(
            encodedPath.constData(),
            name.constData(),
            &acl,
            sizeof(acl),
            0) != 0) {
        nativeError = errno;
        return false;
    }
    nativeError = 0;
    return true;
}

bool linuxAclXattrPresent(
    const QString &path, const QByteArray &name)
{
    const QByteArray encodedPath = QFile::encodeName(path);
    return ::getxattr(
               encodedPath.constData(),
               name.constData(),
               nullptr,
               0) >= 0;
}
#endif

#ifdef Q_OS_MACOS
bool installCurrentUserExtendedAcl(const QString &path)
{
    const int descriptor = ::open(
        QFile::encodeName(path).constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return false;

    uuid_t userUuid;
    if (::mbr_uid_to_uuid(::geteuid(), userUuid) != 0) {
        ::close(descriptor);
        return false;
    }

    acl_t acl = ::acl_init(1);
    if (!acl) {
        ::close(descriptor);
        return false;
    }

    acl_entry_t entry = nullptr;
    acl_permset_t permissions = nullptr;
    acl_flagset_t flags = nullptr;
    const bool installed =
        ::acl_create_entry(&acl, &entry) == 0
        && ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0
        && ::acl_set_qualifier(entry, userUuid) == 0
        && ::acl_get_permset(entry, &permissions) == 0
        && ::acl_clear_perms(permissions) == 0
        && ::acl_add_perm(permissions, ACL_LIST_DIRECTORY) == 0
        && ::acl_set_permset(entry, permissions) == 0
        && ::acl_get_flagset_np(entry, &flags) == 0
        && ::acl_clear_flags_np(flags) == 0
        && ::acl_set_flagset_np(entry, flags) == 0
        && ::acl_set_fd_np(
            descriptor, acl, ACL_TYPE_EXTENDED) == 0;
    ::acl_free(acl);

    errno = 0;
    acl = installed
        ? ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED)
        : nullptr;
    acl_entry_t retainedEntry = nullptr;
    const bool present = acl
        && ::acl_get_entry(
            acl, ACL_FIRST_ENTRY, &retainedEntry) == 0;
    if (acl) ::acl_free(acl);
    ::close(descriptor);
    return installed && present;
}

bool extendedAclIsAbsent(const QString &path)
{
    const int descriptor = ::open(
        QFile::encodeName(path).constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return false;
    errno = 0;
    acl_t acl = ::acl_get_fd_np(
        descriptor, ACL_TYPE_EXTENDED);
    const int aclError = errno;
    if (acl) ::acl_free(acl);
    ::close(descriptor);
    return !acl && aclError == ENOENT;
}
#endif

#ifdef Q_OS_WIN
class WindowsTestHandle
{
public:
    explicit WindowsTestHandle(
        HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~WindowsTestHandle()
    {
        if (isValid()) ::CloseHandle(handle_);
    }

    bool isValid() const
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool setWindowsCreationTime(const QString &path, quint64 ticks)
{
    const QString nativePath = QDir::toNativeSeparators(path);
    WindowsTestHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.isValid()) return false;
    const FILETIME creation {
        DWORD(ticks & 0xffffffffULL),
        DWORD(ticks >> 32)};
    return ::SetFileTime(handle.get(), &creation, nullptr, nullptr);
}

bool createWindowsDirectoryJunction(
    const QString &target,
    const QString &link,
    DWORD &nativeError)
{
    struct JunctionReparseData {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };

    const QString printName = QDir::toNativeSeparators(
        QFileInfo(target).absoluteFilePath());
    const QString substituteName = printName.startsWith(QStringLiteral("\\\\"))
        ? QStringLiteral("\\??\\UNC\\") + printName.mid(2)
        : QStringLiteral("\\??\\") + printName;
    const qsizetype substituteBytes =
        substituteName.size() * qsizetype(sizeof(WCHAR));
    const qsizetype printBytes =
        printName.size() * qsizetype(sizeof(WCHAR));
    const qsizetype pathBytes = substituteBytes + sizeof(WCHAR)
        + printBytes + sizeof(WCHAR);
    const qsizetype totalBytes =
        qsizetype(offsetof(JunctionReparseData, pathBuffer))
        + pathBytes;
    if (totalBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        nativeError = ERROR_BUFFER_OVERFLOW;
        return false;
    }

    const QString nativeLink = QDir::toNativeSeparators(link);
    if (!::CreateDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()), nullptr)) {
        nativeError = ::GetLastError();
        return false;
    }

    QByteArray storage(totalBytes, '\0');
    auto *data = reinterpret_cast<JunctionReparseData *>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = WORD(totalBytes - 8);
    data->substituteNameLength = WORD(substituteBytes);
    data->printNameOffset = WORD(substituteBytes + sizeof(WCHAR));
    data->printNameLength = WORD(printBytes);
    char *pathBuffer = reinterpret_cast<char *>(data->pathBuffer);
    std::memcpy(
        pathBuffer, substituteName.utf16(), size_t(substituteBytes));
    std::memcpy(
        pathBuffer + data->printNameOffset,
        printName.utf16(), size_t(printBytes));

    WindowsTestHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!handle.isValid()) {
        nativeError = ::GetLastError();
        ::RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
        return false;
    }

    DWORD returned = 0;
    const bool created = ::DeviceIoControl(
        handle.get(),
        FSCTL_SET_REPARSE_POINT,
        data,
        DWORD(totalBytes),
        nullptr,
        0,
        &returned,
        nullptr);
    nativeError = created ? ERROR_SUCCESS : ::GetLastError();
    if (!created) {
        ::RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
    }
    return created;
}

bool currentWindowsTestUserSid(QByteArray &storage, PSID &sid)
{
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    WindowsTestHandle token(rawToken);
    DWORD required = 0;
    ::GetTokenInformation(
        token.get(), TokenUser, nullptr, 0, &required);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        return false;
    }
    storage.resize(int(required));
    if (!::GetTokenInformation(
            token.get(), TokenUser, storage.data(),
            required, &required)) {
        return false;
    }
    auto *user = reinterpret_cast<TOKEN_USER *>(
        storage.data());
    if (!::IsValidSid(user->User.Sid)) return false;
    sid = user->User.Sid;
    return true;
}

bool createCurrentUserOwnedDirectory(
    const QString &path, ACCESS_MASK userAccess)
{
    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsTestUserSid(userStorage, userSid)) return false;

    const DWORD aclSize = DWORD(
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE)
        - sizeof(DWORD) + ::GetLengthSid(userSid));
    QByteArray aclStorage(int(aclSize), '\0');
    auto *acl = reinterpret_cast<PACL>(aclStorage.data());
    SECURITY_DESCRIPTOR descriptor {};
    if (!::InitializeAcl(acl, aclSize, ACL_REVISION)
        || !::AddAccessAllowedAceEx(
            acl, ACL_REVISION,
            CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
            userAccess, userSid)
        || !::InitializeSecurityDescriptor(
            &descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !::SetSecurityDescriptorOwner(
            &descriptor, userSid, FALSE)
        || !::SetSecurityDescriptorDacl(
            &descriptor, TRUE, acl, FALSE)
        || !::SetSecurityDescriptorControl(
            &descriptor, SE_DACL_PROTECTED,
            SE_DACL_PROTECTED)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    const QString native = QDir::toNativeSeparators(path);
    return ::CreateDirectoryW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        &attributes);
}

bool windowsDirectoryHasOwnerOnlyAcl(const QString &path)
{
    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsTestUserSid(userStorage, userSid)) return false;

    const QString native = QDir::toNativeSeparators(path);
    WindowsTestHandle directory(::CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!directory.isValid()) return false;
    DWORD fileSystemFlags = 0;
    if (!::GetVolumeInformationByHandleW(
            directory.get(), nullptr, 0, nullptr, nullptr,
            &fileSystemFlags, nullptr, 0)
        || !(fileSystemFlags & FILE_PERSISTENT_ACLS)) {
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = ::GetSecurityInfo(
        directory.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
    if (securityResult != ERROR_SUCCESS) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool privateSecurity = owner
        && ::EqualSid(owner, userSid)
        && acl
        && acl->AceCount == 1
        && ::GetSecurityDescriptorControl(
            descriptor, &control, &revision)
        && (control & SE_DACL_PROTECTED);
    if (privateSecurity) {
        void *rawAce = nullptr;
        if (!::GetAce(acl, 0, &rawAce)) {
            privateSecurity = false;
        } else {
            auto *header = static_cast<ACE_HEADER *>(rawAce);
            auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
            constexpr BYTE inheritanceFlags =
                OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE
                | INHERITED_ACE | INHERIT_ONLY_ACE
                | NO_PROPAGATE_INHERIT_ACE;
            privateSecurity = header->AceType
                    == ACCESS_ALLOWED_ACE_TYPE
                && (header->AceFlags & inheritanceFlags)
                    == (OBJECT_INHERIT_ACE
                        | CONTAINER_INHERIT_ACE)
                && ::IsValidSid(&ace->SidStart)
                && ::EqualSid(&ace->SidStart, userSid)
                && (ace->Mask & FILE_ALL_ACCESS)
                    == FILE_ALL_ACCESS;
        }
    }
    if (descriptor) ::LocalFree(descriptor);
    return privateSecurity;
}

bool setWindowsDirectoryEveryoneAccess(
    const QString &path, ACCESS_MASK everyoneAccess)
{
    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsTestUserSid(userStorage, userSid)) return false;

    DWORD worldSidSize = SECURITY_MAX_SID_SIZE;
    QByteArray worldStorage(int(worldSidSize), '\0');
    PSID worldSid = worldStorage.data();
    if (!::CreateWellKnownSid(
            WinWorldSid, nullptr, worldSid, &worldSidSize)) {
        return false;
    }

    const DWORD aclSize = DWORD(
        sizeof(ACL)
        + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD))
        + ::GetLengthSid(userSid)
        + ::GetLengthSid(worldSid));
    QByteArray aclStorage(int(aclSize), '\0');
    auto *acl = reinterpret_cast<PACL>(aclStorage.data());
    constexpr DWORD inheritance =
        CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    if (!::InitializeAcl(acl, aclSize, ACL_REVISION)
        || !::AddAccessAllowedAceEx(
            acl, ACL_REVISION, inheritance,
            FILE_ALL_ACCESS, userSid)
        || !::AddAccessAllowedAceEx(
            acl, ACL_REVISION, inheritance,
            everyoneAccess, worldSid)) {
        return false;
    }

    const QString native = QDir::toNativeSeparators(path);
    WindowsTestHandle directory(::CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        READ_CONTROL | WRITE_DAC | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!directory.isValid()) return false;
    return ::SetSecurityInfo(
               directory.get(), SE_FILE_OBJECT,
               DACL_SECURITY_INFORMATION
                   | PROTECTED_DACL_SECURITY_INFORMATION,
               nullptr, nullptr, acl, nullptr)
        == ERROR_SUCCESS;
}

bool makeWindowsDirectoryPermissive(const QString &path)
{
    return setWindowsDirectoryEveryoneAccess(
        path, FILE_GENERIC_READ | FILE_TRAVERSE);
}

bool makeWindowsDirectoryEveryoneWritable(const QString &path)
{
    return setWindowsDirectoryEveryoneAccess(
        path, FILE_ALL_ACCESS);
}
#endif

bool makeOwnedDirectoryPermissive(const QString &path)
{
#ifdef Q_OS_UNIX
    return ::chmod(
               QFile::encodeName(path).constData(), 0755) == 0;
#elif defined(Q_OS_WIN)
    return makeWindowsDirectoryPermissive(path);
#else
    Q_UNUSED(path)
    return false;
#endif
}

} // namespace

void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &primary,
    const QString &secondary)
{
    if (filesystemAction) filesystemAction(
        transition, primary, secondary);
}

bool anchoredFilesystemSyncFailureRequested(const QString &)
{
    return failDirectorySync;
}

bool anchoredFilesystemFileUnlinkFailureRequested(const QString &)
{
    return failFileUnlink;
}

bool anchoredFilesystemUseLegacyWindowsDelete()
{
    return forceLegacyWindowsDelete;
}

bool anchoredFilesystemForceZeroWindowsFileId()
{
    return forceZeroWindowsFileId;
}

namespace {

void writeFixture(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(file.write(contents), qint64(contents.size()));
    QVERIFY(file.flush());
}

QByteArray readFixture(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool createHardLink(const QString &source, const QString &target)
{
#ifdef Q_OS_UNIX
    return ::link(QFile::encodeName(source).constData(),
                  QFile::encodeName(target).constData()) == 0;
#elif defined(Q_OS_WIN)
    return ::CreateHardLinkW(
        reinterpret_cast<LPCWSTR>(target.utf16()),
        reinterpret_cast<LPCWSTR>(source.utf16()),
        nullptr);
#else
    Q_UNUSED(source)
    Q_UNUSED(target)
    return false;
#endif
}

#ifdef Q_OS_UNIX
struct NativeFileTimes
{
    timespec accessed {};
    timespec modified {};
};

bool captureNativeFileTimes(
    const QString &path,
    NativeFileTimes &times)
{
    struct stat status {};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0)
        return false;
#ifdef Q_OS_MACOS
    times.accessed = status.st_atimespec;
    times.modified = status.st_mtimespec;
#else
    times.accessed = status.st_atim;
    times.modified = status.st_mtim;
#endif
    return true;
}

bool restoreNativeFileTimes(
    const QString &path,
    const NativeFileTimes &times)
{
    const timespec values[] = {times.accessed, times.modified};
    return ::utimensat(
        AT_FDCWD,
        QFile::encodeName(path).constData(),
        values,
        0) == 0;
}
#endif

bool createSymbolicLink(const QString &source, const QString &target)
{
#ifdef Q_OS_UNIX
    return ::symlink(QFile::encodeName(source).constData(),
                     QFile::encodeName(target).constData()) == 0;
#elif defined(Q_OS_WIN)
    return ::CreateSymbolicLinkW(
        reinterpret_cast<LPCWSTR>(target.utf16()),
        reinterpret_cast<LPCWSTR>(source.utf16()),
        0);
#else
    Q_UNUSED(source)
    Q_UNUSED(target)
    return false;
#endif
}

bool renameFixture(const QString &source, const QString &target)
{
#ifdef Q_OS_WIN
    const QString nativeSource = QDir::toNativeSeparators(source);
    const QString nativeTarget = QDir::toNativeSeparators(target);
    return ::MoveFileExW(
        reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
        reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
        MOVEFILE_WRITE_THROUGH);
#else
    return QFile::rename(source, target);
#endif
}

DirectoryAnchor openDirectory(const QString &path)
{
    DirectoryAnchor directory;
    QString error;
    if (!DirectoryAnchor::open(path, directory, error)) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return directory;
}

DirectoryAnchor openPrivateDirectory(const QString &path)
{
    DirectoryAnchor directory = openDirectory(path);
    QString error;
    if (!hardenPrivateDirectory(directory, error)) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return directory;
}

EntryRef entry(const DirectoryAnchor &directory, const QString &name)
{
    QString error;
    EntryRef result = directory.entry(name, error);
    if (!result.isValid()) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return result;
}

PinnedFile pin(const EntryRef &reference)
{
    PinnedFile file;
    QString error;
    if (!pinRegularFile(reference, file, error)) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return file;
}

void verifyApplied(const MutationResult &result)
{
    QVERIFY2(result.applied(), qPrintable(result.error));
    QVERIFY(result.effect == MutationEffect::AppliedDurable
            || result.effect == MutationEffect::AppliedNotDurable);
}

void verifyPinnedAt(
    PinnedFile &file,
    const EntryRef &entry,
    const NativeIdentity &expected)
{
    bool matches = false;
    QString error;
    QVERIFY(entryMatches(entry, file, matches, error));
    QVERIFY2(matches, qPrintable(error));
    QCOMPARE(file.identity(), expected);
    file = {};
    QCOMPARE(pin(entry).identity(), expected);
}

} // namespace

class TestAnchoredFilesystem : public QObject
{
    Q_OBJECT

private slots:
    void persistentFingerprintIncludesImmutableGenerationEvidence();
    void rejectsUnsafeComponents_data();
    void rejectsUnsafeComponents();
    void rejectsUnsafeFileTypes();
    void opensTrustedRootDirectoryAlias();
    void rejectsUserControlledDirectoryAlias();
    void pinsIdentityAndContentThroughOneHandle();
    void permitsConcurrentPinsOfOneIdentity();
#ifdef Q_OS_LINUX
    void fileGenerationGuardAnchorsWatchToPinnedFile();
#endif
    void permitsOrdinaryQtReadsWhilePinned();
    void permitsOrdinaryQtReadsOfPinnedCopy();
    void newAtomicWriterHandsOffStagingPin();
    void newAtomicWriterRejectsNameReplacement_data();
    void newAtomicWriterRejectsNameReplacement();
    void newAtomicWriterRejectsPrePublishReplacement();
#ifdef Q_OS_UNIX
    void newAtomicWriterRejectsPostPublishParentReplacement();
    void newAtomicWriterRejectsUnixHandoffRewrite();
    void newAtomicWriterDoesNotClaimDeletedStaging();
#endif
#ifdef Q_OS_WIN
    void newAtomicWriterBlocksWindowsPreBridgeReplacement();
    void newAtomicWriterRetainsWindowsStagingWhenHandoffIsBlocked();
    void newAtomicWriterRejectsWindowsHandoffRace_data();
    void newAtomicWriterRejectsWindowsHandoffRace();
    void outputFilesDenyConcurrentWindowsWrites_data();
    void outputFilesDenyConcurrentWindowsWrites();
    void outputFilesCanBeRepinned_data();
    void outputFilesCanBeRepinned();
    void outputPinFinalizationFailureRetainsFile_data();
    void outputPinFinalizationFailureRetainsFile();
    void outputDigestMismatchRetainsFile_data();
    void outputDigestMismatchRetainsFile();
    void moveAcceptsWindowsRenameOnlyChange();
    void repeatedPrivateHardeningPreservesPinnedChild();
#endif
    void permitsAtomicSiblingReplacementWhileDirectoryAnchored();
    void permitsAtomicReplacementWhilePinned();
    void readsPinnedContentsAfterPathReplacement();
    void streamsPinnedContentsInChunks();
    void verifiedPathRejectsFinalNameReplacement();
    void verifiedPathRejectsPostDigestRewrite();
    void verifiedPathRejectsFinalParentReplacement();
    void directoryAnchorSurvivesPathReplacement();
    void directoryAnchorDetectsPathReplacement();
    void childAnchorsRemainInOneRootGeneration();
    void optionalChildOpenDistinguishesMissingAndUnsafeEntries();
    void rejectsUnavailableWindowsFileIdentity();
    void inspectsEntriesThroughPinnedDirectory();
    void enumeratesEntriesFromPinnedDirectoryGeneration();
    void enumeratesWindowsDirectoryAcrossNativeBuffers();
    void directoryEnumerationRejectsUnsafeTypes();
    void directoryEnumerationRejectsWindowsJunction();
    void directoryEnumerationRejectsMutationBetweenPasses();
    void directoryEnumerationRejectsSameSizeRewriteBetweenPasses();
    void directoryEnumerationEnforcesEntryBudget();
    void permitsConcurrentDirectoryEnumerations();
    void enumeratedIdentityDetectsFollowupReplacement();
    void copiesPinnedContentsThroughAnchoredParents();
    void copyDoesNotReplaceDestination();
    void copyReportsNonDurableCleanup();
    void replaceExchangesPinnedGenerations();
    void replaceReportsTargetSubstitutionAtMutation();
    void replaceReportsStagingSubstitutionAtMutation();
    void replaceDoesNotAttemptUnverifiedRollback();
    void moveDoesNotRestoreUnverifiedDestination();
    void moveDoesNotReportModifiedRecoveryPath();
    void moveRejectsReplacementAfterDirectorySync();
    void moveRejectsNewHardLink();
    void moveUsesPinnedParentAfterPathReplacement();
    void moveRejectsFinalEntryReplacement();
    void moveDoesNotReplaceDestination();
    void moveDoesNotReplaceDestinationAcrossDirectories();
    void removeRejectsPinnedParentPathReplacement();
    void removeRejectsFinalEntryReplacement();
    void hardensPrivateDirectory();
    void hardensCurrentUserOwnedDirectoryWithoutWriteOwner();
    void hardensPrivateDirectoryAcls_data();
    void hardensPrivateDirectoryAcls();
    void validatesCurrentUserControlledParent();
    void rejectsWritableCurrentUserOwnedParent();
    void acceptsPrivateGroupWritableParent();
    void createsPrivateChildDirectory();
    void createsPrivateFixedChildInPermissiveParent();
    void strictPrivateChildRejectsPermissiveParent();
    void privateFixedChildRejectsParentAcl();
    void privateFixedChildRejectsCollision();
    void privateFixedChildReportsSyncFailure_data();
    void privateFixedChildReportsSyncFailure();
    void privateChildCreationRejectsCollision();
    void privateChildCreationRejectsPublishCollision();
    void privateChildCreationRejectsReplacedParent();
    void privateChildCreationRejectsStagingReplacement();
    void privateChildCreationRetainsNonEmptyStaging();
    void privateChildCreationRejectsPublishedReplacement_data();
    void privateChildCreationRejectsPublishedReplacement();
    void privateChildCreationReportsParentSyncFailure();
    void removesAnchoredEmptyDirectory();
    void emptyDirectoryRemovalRejectsReplacedParent();
    void emptyDirectoryRemovalRetainsPreQuarantineReplacement();
    void emptyDirectoryRemovalRetainsFinalNameReplacement();
    void emptyDirectoryRemovalRetainsNonEmptyQuarantine();
    void emptyDirectoryRemovalRejectsRepopulatedOriginalName();
    void emptyDirectoryRemovalReportsParentSyncFailure();
    void emptyDirectoryRemovalReportsPartialSyncFailure();
    void emptyDirectoryRemovalCanRetryAfterNonEmptyFailure();
    void emptyDirectoryRemovalRetriesAfterDeleteSharingConflict();
    void emptyDirectoryRemovalRejectsWindowsRepopulation();
    void emptyDirectoryRemovalRejectsWindowsAliases();
    void emptyDirectoryRemovalUnlinksWindowsNameWithSharedObserver();
    void emptyDirectoryRemovalLegacyWindowsDeleteReportsPendingName();
    void removeRetainsReplacementAtQuarantine();
    void removePartialMoveReportsGeneratedQuarantine();
    void removeDetectsReplacementAfterFinalQuarantineCheck();
    void removeUnlinksWindowsNameWithSharedObserver();
    void removeLegacyWindowsDeleteReportsPendingName();
    void syncsPinnedDirectory();
};

void TestAnchoredFilesystem::
persistentFingerprintIncludesImmutableGenerationEvidence()
{
    const QByteArray reusedNativeKey("f:1:2");
    const NativeIdentity original(
        reusedNativeKey, 1, QByteArray("birth-generation-1"));
    const NativeIdentity reused(
        reusedNativeKey, 1, QByteArray("birth-generation-2"));

    QVERIFY(original != reused);
    QVERIFY(!original.persistentFingerprint().isEmpty());
    QVERIFY(!reused.persistentFingerprint().isEmpty());
    QVERIFY(original.persistentFingerprint()
        != reused.persistentFingerprint());
}

void TestAnchoredFilesystem::rejectsUnsafeComponents_data()
{
    QTest::addColumn<QString>("component");
    QTest::newRow("empty") << QString();
    QTest::newRow("dot") << QStringLiteral(".");
    QTest::newRow("dot-dot") << QStringLiteral("..");
    QTest::newRow("slash") << QStringLiteral("one/two");
    QTest::newRow("backslash") << QStringLiteral("one\\two");
    QTest::newRow("absolute") << QDir::rootPath();
}

void TestAnchoredFilesystem::rejectsUnsafeComponents()
{
    QFETCH(QString, component);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());

    QString error;
    const EntryRef reference = directory.entry(component, error);
    QVERIFY(!reference.isValid());
    QVERIFY(!error.isEmpty());
}

void TestAnchoredFilesystem::rejectsUnsafeFileTypes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());

    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("directory")));
    QString error;
    PinnedFile pinned;
    QVERIFY(!pinRegularFile(
        entry(directory, QStringLiteral("directory")), pinned, error));
    QVERIFY(!error.isEmpty());

    const QString ordinary = root.filePath(QStringLiteral("ordinary"));
    const QString hardLink = root.filePath(QStringLiteral("hard-link"));
    writeFixture(ordinary, QByteArray("fixture"));
    if (createHardLink(ordinary, hardLink)) {
        error.clear();
        QVERIFY(!pinRegularFile(
            entry(directory, QStringLiteral("ordinary")), pinned, error));
        QVERIFY(!error.isEmpty());
    }

    const QString symbolic = root.filePath(QStringLiteral("symbolic"));
    if (createSymbolicLink(ordinary, symbolic)) {
        error.clear();
        QVERIFY(!pinRegularFile(
            entry(directory, QStringLiteral("symbolic")), pinned, error));
        QVERIFY(!error.isEmpty());
    }
}

void TestAnchoredFilesystem::opensTrustedRootDirectoryAlias()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX root aliases are required");
#elif defined(Q_OS_MACOS)
    const QFileInfo systemAlias(QStringLiteral("/var"));
    QVERIFY(systemAlias.isSymLink());
    QTemporaryDir target(
        QStringLiteral("/var/tmp/gc-anchored-root-alias-XXXXXX"));
    QVERIFY(target.isValid());

    DirectoryAnchor directory;
    QString error;
    QVERIFY2(
        DirectoryAnchor::open(target.path(), directory, error),
        qPrintable(error));
    QCOMPARE(
        directory.displayPath(),
        QFileInfo(target.path()).canonicalFilePath());
#else
    if (::geteuid() != 0)
        QSKIP("Creating a trusted root alias requires root");

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const QString alias = QStringLiteral(
        "/gc-anchored-root-alias-%1")
        .arg(QCoreApplication::applicationPid());
    QVERIFY(!QFileInfo::exists(alias));
    QVERIFY(createSymbolicLink(target.path(), alias));

    DirectoryAnchor directory;
    QString error;
    const bool opened = DirectoryAnchor::open(
        alias, directory, error);
    const bool removed = QFile::remove(alias);
    QVERIFY(removed);
    QVERIFY2(opened, qPrintable(error));
    QCOMPARE(
        directory.displayPath(),
        QFileInfo(target.path()).canonicalFilePath());
#endif
}

void TestAnchoredFilesystem::rejectsUserControlledDirectoryAlias()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX symbolic links are required");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString canonicalRoot =
        QFileInfo(root.path()).canonicalFilePath();
    QVERIFY(!canonicalRoot.isEmpty());
    const QString target = QDir(canonicalRoot).filePath(
        QStringLiteral("target"));
    const QString alias = QDir(canonicalRoot).filePath(
        QStringLiteral("alias"));
    QVERIFY(QDir().mkdir(target));
    QVERIFY(createSymbolicLink(target, alias));

    DirectoryAnchor directory;
    QString error;
    QVERIFY(!DirectoryAnchor::open(alias, directory, error));
    QVERIFY(!directory.isValid());
    QVERIFY(!error.isEmpty());
#endif
}

void TestAnchoredFilesystem::pinsIdentityAndContentThroughOneHandle()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray contents("same-content replacement");
    writeFixture(source.displayPath(), contents);

    const PinnedFile original = pin(source);
    QVERIFY(original.identity().isValid());
    QCOMPARE(original.identity().linkCount(), quint64(1));
    QCOMPARE(original.size(), qint64(contents.size()));
    QCOMPARE(original.sha256(), QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256));

    const bool replaced = QFile::rename(
        source.displayPath(), root.filePath(QStringLiteral("retained")));
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        bool matches = false;
        QString error;
        QVERIFY(entryMatches(source, original, matches, error));
        QVERIFY2(matches, qPrintable(error));
        return;
    }
    writeFixture(source.displayPath(), contents);
    const PinnedFile substitute = pin(source);
    QVERIFY(original.identity() != substitute.identity());

    bool matches = true;
    QString error;
    QVERIFY(entryMatches(source, original, matches, error));
    QVERIFY(!matches);
    QVERIFY(error.isEmpty());
}

void TestAnchoredFilesystem::permitsConcurrentPinsOfOneIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    writeFixture(source.displayPath(), QByteArray("fixture"));

    const PinnedFile first = pin(source);
    PinnedFile second;
    QString error;
    QVERIFY2(
        pinRegularFile(source, second, error),
        qPrintable(error));
    QCOMPARE(second.identity(), first.identity());
}

#ifdef Q_OS_LINUX
void TestAnchoredFilesystem::fileGenerationGuardAnchorsWatchToPinnedFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString malicious = root.filePath(QStringLiteral("malicious"));
    const QString retainedLive =
        root.filePath(QStringLiteral("retained-live"));
    const QString retainedMalicious =
        root.filePath(QStringLiteral("retained-malicious"));
    QVERIFY(QDir().mkdir(live));
    QVERIFY(QDir().mkdir(malicious));

    const DirectoryAnchor directory = openDirectory(live);
    const EntryRef source = entry(directory, QStringLiteral("source"));
    writeFixture(source.displayPath(), QByteArray("trusted"));
    writeFixture(
        QDir(malicious).filePath(QStringLiteral("source")),
        QByteArray("substitute"));
    const PinnedFile pinned = pin(source);

    bool beforeWatchReached = false;
    bool watchInstalledReached = false;
    filesystemAction = [&](const char *event,
                           const QString &,
                           const QString &) {
        if (std::strcmp(
                event,
                "file-generation-guard-before-watch") == 0) {
            QVERIFY(renameFixture(live, retainedLive));
            QVERIFY(renameFixture(malicious, live));
            beforeWatchReached = true;
        } else if (std::strcmp(
                       event,
                       "file-generation-guard-watch-installed") == 0) {
            QVERIFY(renameFixture(live, retainedMalicious));
            QVERIFY(renameFixture(retainedLive, live));
            watchInstalledReached = true;
        }
    };

    FileGenerationGuard guard;
    QString error;
    QVERIFY2(
        guardFileGeneration(source, pinned, guard, error),
        qPrintable(error));
    filesystemAction = {};
    QVERIFY(beforeWatchReached);
    QVERIFY(watchInstalledReached);

    const QString retainedSource =
        QDir(live).filePath(QStringLiteral("retained-source"));
    QVERIFY(renameFixture(source.displayPath(), retainedSource));
    writeFixture(source.displayPath(), QByteArray("replacement"));

    QVERIFY(!guard.unchanged(error));
    QVERIFY(!error.isEmpty());
}
#endif

void TestAnchoredFilesystem::permitsOrdinaryQtReadsWhilePinned()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray contents("fixture");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinned = pin(source);
    QFile ordinaryReader(source.displayPath());
    QVERIFY2(
        ordinaryReader.open(QIODevice::ReadOnly),
        qPrintable(ordinaryReader.errorString()));
    QCOMPARE(ordinaryReader.readAll(), contents);
}

void TestAnchoredFilesystem::permitsOrdinaryQtReadsOfPinnedCopy()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("fixture");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    PinnedFile pinnedCopy;
    QString error;
    QVERIFY2(
        copyToNewFile(
            pinnedSource, destination, pinnedCopy, error),
        qPrintable(error));

    QFile ordinaryReader(destination.displayPath());
    QVERIFY2(
        ordinaryReader.open(QIODevice::ReadOnly),
        qPrintable(ordinaryReader.errorString()));
    QCOMPARE(ordinaryReader.readAll(), contents);
}

void TestAnchoredFilesystem::newAtomicWriterHandsOffStagingPin()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("complete activity");

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    QVERIFY2(writer.commit(), qPrintable(writer.errorString()));
    QCOMPARE(readFixture(target), contents);
}

void TestAnchoredFilesystem::newAtomicWriterRejectsNameReplacement_data()
{
    QTest::addColumn<QString>("transition");

#ifndef Q_OS_WIN
    QTest::newRow("before-bridge")
        << QStringLiteral("writer-pin-identity-captured");
#endif
    QTest::newRow("after-writer-release")
        << QStringLiteral("output-pin-writer-released");
}

void TestAnchoredFilesystem::newAtomicWriterRejectsNameReplacement()
{
    QFETCH(QString, transition);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QString retained = root.filePath(QStringLiteral("retained.tmp"));
    const QByteArray contents("complete activity");
    int publisherCalls = 0;
    const PinnedAtomicPublishFunction publisher = [&publisherCalls](
            AnchoredFileSystem::PinnedFile &staging,
            const AnchoredFileSystem::EntryRef &published,
            bool &targetPublished, QString &error) {
        ++publisherCalls;
        return publishPinnedAtomicNew(
            staging, published, targetPublished, error);
    };
    bool hookReached = false;
    bool replacementCreated = false;
    QString stagingPath;

    NewAtomicFileWriter writer(target, publisher);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *event,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || QString::fromLatin1(event) != transition) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        if (!renameFixture(primary, retained)) return;
        writeFixture(primary, contents);
        replacementCreated = true;
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(replacementCreated);
    QVERIFY(!committed);
    QCOMPARE(publisherCalls, 0);
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(readFixture(stagingPath), contents);
    QCOMPARE(readFixture(retained), contents);
    QVERIFY(writer.errorString().contains(
        QStringLiteral("writer released"), Qt::CaseInsensitive));
    const QString expectedReason = transition
            == QStringLiteral("writer-pin-identity-captured")
        ? QStringLiteral("before it was pinned")
        : QStringLiteral("while its writer was released");
    QVERIFY(writer.errorString().contains(expectedReason));
    QVERIFY(!writer.errorString().contains(stagingPath));
}

void TestAnchoredFilesystem::newAtomicWriterRejectsPrePublishReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QString retained = root.filePath(QStringLiteral("retained.tmp"));
    const QByteArray contents("complete activity");
    bool hookReached = false;
    bool replacementCreated = false;
    QString stagingPath;

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || qstrcmp(transition, "move-before-publish") != 0) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        if (!renameFixture(primary, retained)) return;
        writeFixture(primary, contents);
        replacementCreated = true;
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(replacementCreated);
    QVERIFY(!committed);
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(readFixture(stagingPath), contents);
    QCOMPARE(readFixture(retained), contents);
    QVERIFY(writer.errorString().contains(
        QStringLiteral("replaced"), Qt::CaseInsensitive));
}

#ifdef Q_OS_UNIX

void TestAnchoredFilesystem::
newAtomicWriterRejectsPostPublishParentReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString liveDirectory =
        root.filePath(QStringLiteral("live"));
    const QString retainedDirectory =
        root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(liveDirectory));
    const QString target = QDir(liveDirectory).filePath(
        QStringLiteral("activity.json"));
    const QByteArray contents("complete activity");
    const QByteArray replacement("concurrent activity");
    bool hookReached = false;

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &) {
        if (hookReached
            || qstrcmp(transition, "move-before-publish") != 0) {
            return;
        }
        hookReached = true;
        QVERIFY(QDir().rename(liveDirectory, retainedDirectory));
        QVERIFY(QDir().mkdir(liveDirectory));
        writeFixture(target, replacement);
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY2(!committed,
             "Publication succeeded through a replaced parent path");
    QVERIFY(writer.errorString().contains(
        QStringLiteral("directory"), Qt::CaseInsensitive));
    QVERIFY(writer.errorString().contains(
        QStringLiteral("replaced"), Qt::CaseInsensitive));
    QCOMPARE(readFixture(target), replacement);
    const QString retainedActivity = QDir(retainedDirectory).filePath(
        QStringLiteral("activity.json"));
    QCOMPARE(readFixture(retainedActivity), contents);
    QVERIFY(!writer.errorString().contains(retainedActivity));
}

void TestAnchoredFilesystem::newAtomicWriterRejectsUnixHandoffRewrite()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("complete activity");
    const QByteArray replacement("modified activity");
    QCOMPARE(replacement.size(), contents.size());
    int publisherCalls = 0;
    const PinnedAtomicPublishFunction publisher = [&publisherCalls](
            AnchoredFileSystem::PinnedFile &,
            const AnchoredFileSystem::EntryRef &,
            bool &, QString &) {
        ++publisherCalls;
        return true;
    };
    bool hookReached = false;
    bool replacementWritten = false;
    QString stagingPath;

    NewAtomicFileWriter writer(target, publisher);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || qstrcmp(transition, "output-pin-writer-released") != 0) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        QFile replacementFile(primary);
        if (!replacementFile.open(QIODevice::ReadWrite)
            || !replacementFile.seek(0)) {
            return;
        }
        replacementWritten =
            replacementFile.write(replacement)
                == qint64(replacement.size())
            && replacementFile.flush();
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(replacementWritten);
    QVERIFY(!committed);
    QCOMPARE(publisherCalls, 0);
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(readFixture(stagingPath), replacement);
    QVERIFY(writer.errorString().contains(
        QStringLiteral("changed"), Qt::CaseInsensitive));
    QVERIFY(!writer.errorString().contains(stagingPath));
}

void TestAnchoredFilesystem::newAtomicWriterDoesNotClaimDeletedStaging()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("complete activity");
    bool hookReached = false;
    QString stagingPath;

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || qstrcmp(transition, "output-pin-writer-released") != 0) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        QVERIFY(QFile::remove(primary));
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(!committed);
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(!QFileInfo::exists(stagingPath));
    QVERIFY(writer.errorString().contains(
        QStringLiteral("writer released"), Qt::CaseInsensitive));
    QVERIFY(!writer.errorString().contains(
        QStringLiteral("data retained"), Qt::CaseInsensitive));
    QVERIFY(!writer.errorString().contains(stagingPath));
}

#endif

#ifdef Q_OS_WIN

void TestAnchoredFilesystem::
newAtomicWriterBlocksWindowsPreBridgeReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QString retained = root.filePath(QStringLiteral("retained.tmp"));
    const QByteArray contents("complete activity");
    bool hookReached = false;
    bool renamed = false;
    DWORD renameError = ERROR_SUCCESS;

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || qstrcmp(
                transition, "writer-pin-identity-captured") != 0) {
            return;
        }
        hookReached = true;
        const QString nativeSource = QDir::toNativeSeparators(primary);
        const QString nativeTarget = QDir::toNativeSeparators(retained);
        renamed = ::MoveFileExW(
            reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
            reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
            MOVEFILE_WRITE_THROUGH);
        renameError = renamed ? ERROR_SUCCESS : ::GetLastError();
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(!renamed);
    QCOMPARE(renameError, DWORD(ERROR_SHARING_VIOLATION));
    QVERIFY2(committed, qPrintable(writer.errorString()));
    QCOMPARE(readFixture(target), contents);
    QVERIFY(!QFileInfo::exists(retained));
}

void TestAnchoredFilesystem::
newAtomicWriterRetainsWindowsStagingWhenHandoffIsBlocked()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("recoverable activity");
    bool hookReached = false;
    QString stagingPath;
    std::unique_ptr<WindowsTestHandle> concurrentWriter;

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || qstrcmp(transition, "output-pin-writer-released") != 0) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        concurrentWriter = std::make_unique<WindowsTestHandle>(
            ::CreateFileW(
                reinterpret_cast<LPCWSTR>(primary.utf16()),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(concurrentWriter && concurrentWriter->isValid());
    QVERIFY(!committed);
    QVERIFY(writer.errorString().contains(
        QStringLiteral("system error 32"), Qt::CaseInsensitive));
    QVERIFY(writer.errorString().contains(
        QStringLiteral("writer released"), Qt::CaseInsensitive));
    QVERIFY(!writer.errorString().contains(stagingPath));
    QVERIFY(!QFileInfo::exists(target));
    concurrentWriter.reset();
    QVERIFY(QFileInfo::exists(stagingPath));
    QCOMPARE(readFixture(stagingPath), contents);
}

void TestAnchoredFilesystem::newAtomicWriterRejectsWindowsHandoffRace_data()
{
    QTest::addColumn<QString>("race");
    QTest::addColumn<QString>("transition");

    QTest::newRow("path-replacement")
        << QStringLiteral("replace")
        << QStringLiteral("output-pin-writer-released");
    QTest::newRow("same-object-rewrite")
        << QStringLiteral("rewrite")
        << QStringLiteral("output-pin-writer-released");
    QTest::newRow("same-object-rewrite-before-bridge")
        << QStringLiteral("rewrite")
        << QStringLiteral("writer-pin-identity-captured");
}

void TestAnchoredFilesystem::newAtomicWriterRejectsWindowsHandoffRace()
{
    QFETCH(QString, race);
    QFETCH(QString, transition);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QString retained = root.filePath(QStringLiteral("retained.tmp"));
    const QByteArray contents("complete activity");
    const QByteArray replacement("modified activity");
    QCOMPARE(replacement.size(), contents.size());
    int publisherCalls = 0;
    const PinnedAtomicPublishFunction publisher = [&publisherCalls](
            AnchoredFileSystem::PinnedFile &,
            const AnchoredFileSystem::EntryRef &,
            bool &, QString &) {
        ++publisherCalls;
        return true;
    };
    bool hookReached = false;
    bool raceApplied = false;
    QString stagingPath;

    NewAtomicFileWriter writer(target, publisher);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    filesystemAction = [&](const char *event,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || QString::fromLatin1(event) != transition) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        if (race == QStringLiteral("replace")) {
            const QString nativeSource = QDir::toNativeSeparators(primary);
            const QString nativeTarget = QDir::toNativeSeparators(retained);
            raceApplied = ::MoveFileExW(
                reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
                reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                MOVEFILE_WRITE_THROUGH);
            if (raceApplied) writeFixture(primary, replacement);
            return;
        }

        WindowsTestHandle concurrentWriter(::CreateFileW(
            reinterpret_cast<LPCWSTR>(primary.utf16()),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        LARGE_INTEGER start {};
        DWORD written = 0;
        raceApplied = concurrentWriter.isValid()
            && ::SetFilePointerEx(
                concurrentWriter.get(), start, nullptr, FILE_BEGIN)
            && ::WriteFile(
                concurrentWriter.get(), replacement.constData(),
                DWORD(replacement.size()), &written, nullptr)
            && written == DWORD(replacement.size())
            && ::FlushFileBuffers(concurrentWriter.get());
    };

    const bool committed = writer.commit();
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(raceApplied);
    QVERIFY(!committed);
    QCOMPARE(publisherCalls, 0);
    const QString expectedReason = transition
            == QStringLiteral("writer-pin-identity-captured")
        ? QStringLiteral("before it was pinned")
        : QStringLiteral("while its writer was released");
    QVERIFY(writer.errorString().contains(expectedReason));
    QVERIFY(writer.errorString().contains(
        QStringLiteral("writer released"), Qt::CaseInsensitive));
    QVERIFY(!writer.errorString().contains(stagingPath));
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(readFixture(stagingPath), replacement);
    if (race == QStringLiteral("replace")) {
        QCOMPARE(readFixture(retained), contents);
    }
}

void TestAnchoredFilesystem::
outputFilesDenyConcurrentWindowsWrites_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputFilesDenyConcurrentWindowsWrites()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    QVERIFY2(created, qPrintable(error));

    WindowsTestHandle concurrentWriter(::CreateFileW(
        reinterpret_cast<LPCWSTR>(destination.displayPath().utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    const DWORD nativeError = ::GetLastError();
    QVERIFY2(
        !concurrentWriter.isValid(),
        "A live anchored output pin allowed a concurrent Windows writer");
    QCOMPARE(nativeError, DWORD(ERROR_SHARING_VIOLATION));
}

void TestAnchoredFilesystem::outputFilesCanBeRepinned_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputFilesCanBeRepinned()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    QVERIFY2(created, qPrintable(error));

    PinnedFile repinned;
    QVERIFY2(
        pinRegularFile(destination, repinned, error),
        qPrintable(error));
    QCOMPARE(repinned.identity(), output.identity());
    QCOMPARE(repinned.size(), output.size());
    QCOMPARE(repinned.sha256(), output.sha256());
}

void TestAnchoredFilesystem::
outputPinFinalizationFailureRetainsFile_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputPinFinalizationFailureRetainsFile()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    bool hookReached = false;
    std::unique_ptr<WindowsTestHandle> concurrentWriter;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(transition, "output-pin-writer-released") != 0
            || primary != destination.displayPath()) {
            return;
        }
        hookReached = true;
        concurrentWriter = std::make_unique<WindowsTestHandle>(
            ::CreateFileW(
                reinterpret_cast<LPCWSTR>(primary.utf16()),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
    };

    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(concurrentWriter && concurrentWriter->isValid());
    QVERIFY2(!created, "A sharing-blocked final output pin was accepted");
    QVERIFY2(!error.isEmpty(), "Rejected output must report an error");
    concurrentWriter.reset();
    QVERIFY2(
        QFileInfo::exists(destination.displayPath()),
        "Failed pin finalization deleted a concurrently writable output");
    QCOMPARE(readFixture(destination.displayPath()), contents);
}

void TestAnchoredFilesystem::outputDigestMismatchRetainsFile_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputDigestMismatchRetainsFile()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    const QByteArray replacement(contents.size(), 'x');
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    bool hookReached = false;
    bool writerOpened = false;
    bool replacementWritten = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(transition, "output-pin-writer-released") != 0
            || primary != destination.displayPath()) {
            return;
        }
        hookReached = true;
        WindowsTestHandle writer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(primary.utf16()),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        writerOpened = writer.isValid();
        if (!writerOpened) return;
        LARGE_INTEGER start {};
        DWORD written = 0;
        replacementWritten = ::SetFilePointerEx(
                                 writer.get(), start,
                                 nullptr, FILE_BEGIN)
            && ::WriteFile(
                writer.get(), replacement.constData(),
                DWORD(replacement.size()), &written, nullptr)
            && written == DWORD(replacement.size())
            && ::FlushFileBuffers(writer.get());
    };

    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(writerOpened);
    QVERIFY(replacementWritten);
    QVERIFY2(!created, "A digest-mismatched final output pin was accepted");
    QVERIFY2(!error.isEmpty(), "Rejected output must report an error");
    QVERIFY2(
        QFileInfo::exists(destination.displayPath()),
        "Digest failure deleted a concurrently modified output");
    QCOMPARE(readFixture(destination.displayPath()), replacement);
}
#endif

void TestAnchoredFilesystem::
permitsAtomicSiblingReplacementWhileDirectoryAnchored()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath = root.filePath(
        QStringLiteral(".gc-transactions/linked-removal/journal"));
    const QString plannedPath = root.filePath(
        QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(journalPath));
    QVERIFY(QDir().mkpath(plannedPath));
    const DirectoryAnchor journal = openDirectory(journalPath);
    const QString targetPath = QDir(plannedPath).filePath(
        QStringLiteral("activity.json"));
    writeFixture(targetPath, QByteArray("original"));

    const QByteArray replacement("replacement");
    ReplaceAtomicFileWriter writer(targetPath);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(replacement), qint64(replacement.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    QVERIFY2(writer.commit(), qPrintable(writer.errorString()));

    QCOMPARE(readFixture(targetPath), replacement);
    QString error;
    QVERIFY2(journal.pathMatches(error), qPrintable(error));
}

void TestAnchoredFilesystem::permitsAtomicReplacementWhilePinned()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef target = entry(
        directory, QStringLiteral("activity.json"));
    const QByteArray original("original");
    const QByteArray replacement("replacement");
    writeFixture(target.displayPath(), original);
    const PinnedFile pinned = pin(target);

    ReplaceAtomicFileWriter writer(target.displayPath());
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(replacement), qint64(replacement.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    QVERIFY2(writer.commit(), qPrintable(writer.errorString()));

    QCOMPARE(readFixture(target.displayPath()), replacement);
    QVERIFY(pinned.isValid());
    QCOMPARE(pinned.size(), qint64(original.size()));
}

void TestAnchoredFilesystem::readsPinnedContentsAfterPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray originalContents("original pinned contents");
    const QByteArray substituteContents("substitute contents");
    writeFixture(source.displayPath(), originalContents);
    {
        const PinnedFile original = pin(source);

        QVERIFY(renameFixture(source.displayPath(), retained));
        writeFixture(source.displayPath(), substituteContents);

        QByteArray contents;
        QString error;
        QVERIFY(readAll(original, 1024, contents, error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(contents, originalContents);
        QCOMPARE(readFixture(source.displayPath()), substituteContents);
    }
    QCOMPARE(readFixture(retained), originalContents);
}

void TestAnchoredFilesystem::streamsPinnedContentsInChunks()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QByteArray originalContents(2 * 1024 * 1024 + 17, 'a');
    originalContents[1024 * 1024] = 'b';
    originalContents[originalContents.size() - 1] = 'c';
    writeFixture(source.displayPath(), originalContents);
    const PinnedFile original = pin(source);
    QVERIFY(renameFixture(source.displayPath(), retained));
    writeFixture(source.displayPath(), QByteArray("substitute"));

    QByteArray streamed;
    int chunks = 0;
    QString error;
    QVERIFY(streamContents(
        original,
        [&streamed, &chunks](
            const char *data, qsizetype size, QString &) {
            streamed.append(data, size);
            ++chunks;
            return true;
        },
        error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(chunks > 1);
    QCOMPARE(streamed, originalContents);
    QCOMPARE(readFixture(source.displayPath()), QByteArray("substitute"));

    QVERIFY(!streamContents(
        original,
        [](const char *, qsizetype, QString &consumerError) {
            consumerError = QStringLiteral("consumer rejected chunk");
            return false;
        },
        error));
    QCOMPARE(error, QStringLiteral("consumer rejected chunk"));
}

void TestAnchoredFilesystem::verifiedPathRejectsFinalNameReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("The permission-blocked recovery race is Unix-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original contents");
    const QByteArray substitute("substitute contents");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    QString quarantine;
    bool unlinkBlocked = false;
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(
                transition, "remove-quarantine-finally-verified") == 0) {
            quarantine = primary;
            failFileUnlink = true;
            unlinkBlocked = true;
            return;
        }
        if (hookReached
            || qstrcmp(
                transition, "recovery-path-digest-verified") != 0) {
            return;
        }
        hookReached = true;
        failFileUnlink = false;
        QVERIFY(renameFixture(primary, retained));
        writeFixture(primary, substitute);
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};
    failFileUnlink = false;

    QVERIFY(unlinkBlocked);
    QVERIFY(!quarantine.isEmpty());
    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY2(result.verifiedRecoveryPath.isEmpty(),
             "A replaced name was reported as a verified recovery path");
    QCOMPARE(readFixture(quarantine), substitute);
    QCOMPARE(readFixture(retained), original);
#endif
}

void TestAnchoredFilesystem::verifiedPathRejectsPostDigestRewrite()
{
#ifndef Q_OS_UNIX
    QSKIP("Native timestamp restoration is Unix-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString alias = root.filePath(QStringLiteral("alias"));
    const QByteArray original("verified recovery bytes");
    const QByteArray substitute(original.size(), 'x');
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    NativeFileTimes originalTimes;
    QVERIFY(captureNativeFileTimes(
        source.displayPath(), originalTimes));
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (hookReached
            || qstrcmp(
                transition, "recovery-path-digest-verified") != 0) {
            return;
        }
        hookReached = true;
        QVERIFY(createHardLink(primary, alias));
        QFile file(primary);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(substitute), qint64(substitute.size()));
        QVERIFY(file.flush());
        file.close();
        QVERIFY(QFile::remove(alias));
        QVERIFY(restoreNativeFileTimes(primary, originalTimes));
    };

    const QString verified = verifiedRecoveryPath(pinned, source);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY2(verified.isEmpty(),
             "Post-digest replacement was reported as verified");
    QCOMPARE(readFixture(source.displayPath()), substitute);
#endif
}

void TestAnchoredFilesystem::verifiedPathRejectsFinalParentReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("The anchored parent replacement race is Unix-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString parentPath = root.filePath(QStringLiteral("current"));
    const QString retainedParent =
        root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(parentPath));
    const DirectoryAnchor directory = openDirectory(parentPath);
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original recovery bytes");
    const QByteArray substitute("substitute recovery bytes");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool digestVerified = false;
    bool parentReplaced = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &) {
        if (qstrcmp(
                transition, "recovery-path-digest-verified") == 0) {
            digestVerified = true;
            return;
        }
        if (!digestVerified
            || parentReplaced
            || qstrcmp(
                transition, "recovery-path-parent-verified") != 0) {
            return;
        }
        QVERIFY(QDir().rename(parentPath, retainedParent));
        QVERIFY(QDir().mkdir(parentPath));
        writeFixture(
            QDir(parentPath).filePath(QStringLiteral("source")),
            substitute);
        parentReplaced = true;
    };

    const QString verified = verifiedRecoveryPath(pinned, source);
    filesystemAction = {};

    QVERIFY(digestVerified);
    QVERIFY(parentReplaced);
    QVERIFY2(verified.isEmpty(),
             "A path through a replaced parent was reported as verified");
    QCOMPARE(
        readFixture(QDir(parentPath).filePath(QStringLiteral("source"))),
        substitute);
    QCOMPARE(
        readFixture(QDir(retainedParent).filePath(QStringLiteral("source"))),
        original);
#endif
}

#ifdef Q_OS_WIN

void TestAnchoredFilesystem::moveAcceptsWindowsRenameOnlyChange()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString transient = root.filePath(QStringLiteral("transient"));
    const QByteArray contents("rename-stable contents");
    writeFixture(source.displayPath(), contents);
    PinnedFile pinned = pin(source);
    const NativeIdentity identity = pinned.identity();
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (hookReached
            || qstrcmp(
                transition, "move-before-final-name-check") != 0) {
            return;
        }
        hookReached = true;
        QVERIFY(renameFixture(destination, transient));
        QVERIFY(renameFixture(transient, destination));
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(hookReached);
    verifyApplied(result);
    verifyPinnedAt(pinned, target, identity);
    QCOMPARE(readFixture(target.displayPath()), contents);
    QVERIFY(!QFileInfo::exists(transient));
}

#endif

void TestAnchoredFilesystem::directoryAnchorSurvivesPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));

    const DirectoryAnchor original = openDirectory(live);
    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        QCOMPARE(openDirectory(live).identity(), original.identity());
        return;
    }
    QVERIFY(QDir().mkdir(live));
    const DirectoryAnchor substitute = openDirectory(live);

    QVERIFY(original.identity().isValid());
    QVERIFY(original.identity() != substitute.identity());
    QString error;
    QVERIFY(original.sync(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void TestAnchoredFilesystem::directoryAnchorDetectsPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    const DirectoryAnchor original = openDirectory(live);

    QString error;
    QVERIFY(original.pathMatches(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        QVERIFY(original.pathMatches(error));
        return;
    }
    QVERIFY(QDir().mkdir(live));
    QVERIFY(!original.pathMatches(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void TestAnchoredFilesystem::childAnchorsRemainInOneRootGeneration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString live = temporary.filePath(QStringLiteral("live"));
    const QString retained = temporary.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    QVERIFY(QDir(live).mkdir(QStringLiteral("activities")));
    QVERIFY(QDir(live).mkdir(QStringLiteral("backup")));
    const DirectoryAnchor root = openDirectory(live);

    DirectoryAnchor activities;
    QString error;
    QVERIFY(root.openChild(
        QStringLiteral("activities"), activities, error));

    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (replaced) {
        QVERIFY(QDir().mkdir(live));
        QVERIFY(QDir(live).mkdir(QStringLiteral("backup")));
    }

    DirectoryAnchor backup;
    QVERIFY(root.openChild(QStringLiteral("backup"), backup, error));
    const QString expectedPath = replaced
        ? QDir(retained).filePath(QStringLiteral("backup"))
        : QDir(live).filePath(QStringLiteral("backup"));
    QCOMPARE(backup.identity(), openDirectory(expectedPath).identity());
    QVERIFY(activities.identity().isValid());
    if (replaced) {
        QVERIFY(backup.identity()
                != openDirectory(
                    QDir(live).filePath(QStringLiteral("backup")))
                       .identity());
    }
}

void TestAnchoredFilesystem::
optionalChildOpenDistinguishesMissingAndUnsafeEntries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const DirectoryAnchor root = openDirectory(temporary.path());

    DirectoryAnchor child;
    bool exists = true;
    QString error;
    QVERIFY(root.openChildIfExists(
        QStringLiteral("missing"), child, exists, error));
    QVERIFY(!exists);
    QVERIFY(!child.isValid());
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QVERIFY(QDir(temporary.path()).mkdir(QStringLiteral("present")));
    QVERIFY(root.openChildIfExists(
        QStringLiteral("present"), child, exists, error));
    QVERIFY(exists);
    QVERIFY(child.isValid());

    writeFixture(
        temporary.filePath(QStringLiteral("regular-file")),
        QByteArray("not a directory"));
    QVERIFY(!root.openChildIfExists(
        QStringLiteral("regular-file"), child, exists, error));
    QVERIFY(!exists);
    QVERIFY(!child.isValid());
    QVERIFY(!error.isEmpty());
}

void TestAnchoredFilesystem::rejectsUnavailableWindowsFileIdentity()
{
#ifndef Q_OS_WIN
    QSKIP("Zero Windows file identities are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DirectoryAnchor directory;
    QString error;
    forceZeroWindowsFileId = true;
    const bool opened = DirectoryAnchor::open(
        root.path(), directory, error);
    forceZeroWindowsFileId = false;

    QVERIFY(!opened);
    QVERIFY(!directory.isValid());
    QVERIFY(!error.isEmpty());
#endif
}

void TestAnchoredFilesystem::inspectsEntriesThroughPinnedDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    const DirectoryAnchor directory = openDirectory(live);
    const EntryRef present = entry(directory, QStringLiteral("present"));
    const EntryRef missing = entry(directory, QStringLiteral("missing"));
    writeFixture(present.displayPath(), QByteArray("present"));

    bool exists = false;
    QString error;
    QVERIFY(entryExists(present, exists, error));
    QVERIFY(exists);
    QVERIFY(entryExists(missing, exists, error));
    QVERIFY(!exists);

    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (replaced) {
        QVERIFY(QDir().mkdir(live));
        QVERIFY(!QFileInfo::exists(present.displayPath()));
        QVERIFY(entryExists(present, exists, error));
        QVERIFY(exists);
    }
}

void TestAnchoredFilesystem::
enumeratesEntriesFromPinnedDirectoryGeneration()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    writeFixture(
        QDir(live).filePath(QStringLiteral("z-file")),
        QByteArray("original"));
    QVERIFY(QDir(live).mkdir(QStringLiteral("a-directory")));
    const DirectoryAnchor directory = openDirectory(live);

    const bool replaced = renameFixture(live, retained);
#ifdef Q_OS_WIN
    QVERIFY(!replaced);
#else
    QVERIFY(replaced);
#endif
    if (replaced) {
        QVERIFY(QDir().mkdir(live));
        writeFixture(
            QDir(live).filePath(QStringLiteral("substitute")),
            QByteArray("substitute"));
    }

    QList<DirectoryEntry> entries;
    QString error;
    QVERIFY2(
        directory.enumerateEntries(entries, 16, error),
        qPrintable(error));
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).name, QStringLiteral("a-directory"));
    QCOMPARE(entries.at(0).kind, DirectoryEntryKind::Directory);
    QVERIFY(entries.at(0).identity.isValid());
    QCOMPARE(entries.at(1).name, QStringLiteral("z-file"));
    QCOMPARE(entries.at(1).kind, DirectoryEntryKind::RegularFile);
    QVERIFY(entries.at(1).identity.isValid());
}

void TestAnchoredFilesystem::enumeratesWindowsDirectoryAcrossNativeBuffers()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory query continuation is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    constexpr int entryCount = 220;
    QStringList expected;
    for (int index = 0; index < entryCount; ++index) {
        const QString name = QStringLiteral("%1-%2")
            .arg(index, 4, 10, QLatin1Char('0'))
            .arg(QString(180, QLatin1Char('x')));
        expected.append(name);
        writeFixture(root.filePath(name), QByteArray());
    }
    expected.sort(Qt::CaseSensitive);
    const DirectoryAnchor directory = openDirectory(root.path());

    QList<DirectoryEntry> entries;
    QString error;
    QVERIFY2(
        directory.enumerateEntries(entries, entryCount, error),
        qPrintable(error));
    QCOMPARE(entries.size(), entryCount);
    for (int index = 0; index < entryCount; ++index) {
        QCOMPARE(entries.at(index).name, expected.at(index));
        QCOMPARE(
            entries.at(index).kind,
            DirectoryEntryKind::RegularFile);
    }
#endif
}

void TestAnchoredFilesystem::directoryEnumerationRejectsUnsafeTypes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("target"));
    const QString symbolic = root.filePath(QStringLiteral("symbolic"));
    writeFixture(target, QByteArray("target"));
    if (!createSymbolicLink(target, symbolic)) {
        QSKIP("Symbolic links are unavailable in this test environment");
    }
    const DirectoryAnchor directory = openDirectory(root.path());

    QList<DirectoryEntry> entries;
    QString error;
    QVERIFY(!directory.enumerateEntries(entries, 16, error));
    QVERIFY(entries.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestAnchoredFilesystem::directoryEnumerationRejectsWindowsJunction()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory junctions are platform-specific");
#else
    QTemporaryDir root;
    QTemporaryDir target;
    QVERIFY(root.isValid());
    QVERIFY(target.isValid());
    const QString junction = root.filePath(QStringLiteral("junction"));
    DWORD nativeError = ERROR_SUCCESS;
    QVERIFY2(
        createWindowsDirectoryJunction(
            target.path(), junction, nativeError),
        qPrintable(QStringLiteral(
            "Cannot create the Windows junction: system error %1")
                       .arg(nativeError)));
    const DirectoryAnchor directory = openDirectory(root.path());

    QList<DirectoryEntry> entries;
    QString error;
    QVERIFY(!directory.enumerateEntries(entries, 16, error));
    QVERIFY(entries.isEmpty());
    QVERIFY(!error.isEmpty());
#endif
}

void TestAnchoredFilesystem::
directoryEnumerationRejectsMutationBetweenPasses()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString source = root.filePath(QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    writeFixture(source, QByteArray("original"));
    const DirectoryAnchor directory = openDirectory(root.path());
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(
                transition,
                "directory-enumeration-first-pass") != 0
            || primary != root.path()) {
            return;
        }
        actionReached = true;
        QVERIFY(renameFixture(source, retained));
        writeFixture(source, QByteArray("replacement"));
    };

    QList<DirectoryEntry> entries;
    QString error;
    const bool enumerated = directory.enumerateEntries(
        entries, 16, error);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!enumerated);
    QVERIFY(entries.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestAnchoredFilesystem::
directoryEnumerationRejectsSameSizeRewriteBetweenPasses()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString source = root.filePath(QStringLiteral("source"));
    const QByteArray original("original");
    const QByteArray replacement("modified");
    QCOMPARE(original.size(), replacement.size());
    writeFixture(source, original);
    QFile timestampFile(source);
    QVERIFY(timestampFile.open(QIODevice::ReadWrite));
    QVERIFY(timestampFile.setFileTime(
        QDateTime::fromSecsSinceEpoch(946684800, Qt::UTC),
        QFileDevice::FileModificationTime));
    timestampFile.close();
    const DirectoryAnchor directory = openDirectory(root.path());
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(
                transition,
                "directory-enumeration-first-pass") != 0
            || primary != root.path()) {
            return;
        }
        actionReached = true;
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(replacement), qint64(replacement.size()));
        QVERIFY(file.flush());
        QVERIFY(file.setFileTime(
            QDateTime::fromSecsSinceEpoch(978307200, Qt::UTC),
            QFileDevice::FileModificationTime));
    };

    QList<DirectoryEntry> entries;
    QString error;
    const bool enumerated = directory.enumerateEntries(
        entries, 16, error);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!enumerated);
    QVERIFY(entries.isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFixture(source), replacement);
}

void TestAnchoredFilesystem::directoryEnumerationEnforcesEntryBudget()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    writeFixture(
        root.filePath(QStringLiteral("first")), QByteArray("first"));
    writeFixture(
        root.filePath(QStringLiteral("second")), QByteArray("second"));
    const DirectoryAnchor directory = openDirectory(root.path());

    QList<DirectoryEntry> entries;
    QString error;
    QVERIFY(!directory.enumerateEntries(entries, 1, error));
    QVERIFY(entries.isEmpty());
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY2(
        directory.enumerateEntries(entries, 2, error),
        qPrintable(error));
    QCOMPARE(entries.size(), 2);
}

void TestAnchoredFilesystem::permitsConcurrentDirectoryEnumerations()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (int index = 0; index < 8; ++index) {
        writeFixture(
            root.filePath(QStringLiteral("entry-%1").arg(index)),
            QByteArray::number(index));
    }
    const DirectoryAnchor directory = openDirectory(root.path());
    std::atomic<bool> start {false};
    std::atomic<bool> succeeded {true};
    const auto enumerate = [&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int attempt = 0; attempt < 64; ++attempt) {
            QList<DirectoryEntry> entries;
            QString error;
            if (!directory.enumerateEntries(entries, 16, error)
                || entries.size() != 8) {
                succeeded.store(false, std::memory_order_release);
                return;
            }
        }
    };
    std::thread first(enumerate);
    std::thread second(enumerate);
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    QVERIFY(succeeded.load(std::memory_order_acquire));
}

void TestAnchoredFilesystem::
enumeratedIdentityDetectsFollowupReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString filePath = root.filePath(QStringLiteral("file"));
    const QString retainedFile =
        root.filePath(QStringLiteral("retained-file"));
    const QString directoryPath =
        root.filePath(QStringLiteral("directory"));
    const QString retainedDirectory =
        root.filePath(QStringLiteral("retained-directory"));
    writeFixture(filePath, QByteArray("original"));
    QVERIFY(QDir().mkdir(directoryPath));
    const DirectoryAnchor rootDirectory = openDirectory(root.path());

    QList<DirectoryEntry> entries;
    QString error;
    QVERIFY2(
        rootDirectory.enumerateEntries(entries, 16, error),
        qPrintable(error));
    QCOMPARE(entries.size(), 2);
    const DirectoryEntry listedDirectory = entries.at(0);
    const DirectoryEntry listedFile = entries.at(1);
    QCOMPARE(listedDirectory.name, QStringLiteral("directory"));
    QCOMPARE(listedDirectory.kind, DirectoryEntryKind::Directory);
    QCOMPARE(listedFile.name, QStringLiteral("file"));
    QCOMPARE(listedFile.kind, DirectoryEntryKind::RegularFile);

    QVERIFY(renameFixture(filePath, retainedFile));
    writeFixture(filePath, QByteArray("replacement"));
    QVERIFY(renameFixture(directoryPath, retainedDirectory));
    QVERIFY(QDir().mkdir(directoryPath));

    PinnedFile replacementFile = pin(entry(
        rootDirectory, QStringLiteral("file")));
    QVERIFY(replacementFile.identity() != listedFile.identity);
    DirectoryAnchor replacementDirectory;
    QVERIFY(rootDirectory.openChild(
        QStringLiteral("directory"), replacementDirectory, error));
    QVERIFY(replacementDirectory.identity()
            != listedDirectory.identity);
}

void TestAnchoredFilesystem::copiesPinnedContentsThroughAnchoredParents()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString targetPath = root.filePath(QStringLiteral("target-dir"));
    const QString retainedSource =
        root.filePath(QStringLiteral("retained-source"));
    const QString retainedTarget =
        root.filePath(QStringLiteral("retained-target"));
    QVERIFY(QDir().mkdir(sourcePath));
    QVERIFY(QDir().mkdir(targetPath));

    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const DirectoryAnchor targetDirectory = openDirectory(targetPath);
    const EntryRef source = entry(sourceDirectory, QStringLiteral("source"));
    const EntryRef target = entry(targetDirectory, QStringLiteral("target"));
    const QByteArray originalContents("anchored copy contents");
    const QByteArray substituteContents("substitute source contents");
    writeFixture(source.displayPath(), originalContents);
    PinnedFile pinned = pin(source);

    const bool sourceReplaced = QDir().rename(sourcePath, retainedSource);
    const bool targetReplaced = QDir().rename(targetPath, retainedTarget);
#ifndef Q_OS_WIN
    QVERIFY(sourceReplaced);
    QVERIFY(targetReplaced);
#endif
    if (sourceReplaced) {
        QVERIFY(QDir().mkdir(sourcePath));
        writeFixture(source.displayPath(), substituteContents);
    }
    if (targetReplaced) QVERIFY(QDir().mkdir(targetPath));

    PinnedFile copied;
    QString error;
    QVERIFY(copyToNewFile(pinned, target, copied, error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(copied.size(), qint64(originalContents.size()));
    QCOMPARE(copied.sha256(), QCryptographicHash::hash(
        originalContents, QCryptographicHash::Sha256));
    QByteArray copiedContents;
    QVERIFY(readAll(copied, 1024, copiedContents, error));
    QCOMPARE(copiedContents, originalContents);
    copied = {};
    pinned = {};

    const QString actualTarget = targetReplaced
        ? QDir(retainedTarget).filePath(QStringLiteral("target"))
        : target.displayPath();
    QCOMPARE(readFixture(actualTarget), originalContents);
    if (sourceReplaced) {
        QCOMPARE(readFixture(source.displayPath()), substituteContents);
        QCOMPARE(readFixture(
            QDir(retainedSource).filePath(QStringLiteral("source"))),
            originalContents);
    }
    if (targetReplaced) {
        QVERIFY(!QFileInfo::exists(target.displayPath()));
    }
}

void TestAnchoredFilesystem::copyDoesNotReplaceDestination()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QByteArray sourceContents("source");
    const QByteArray targetContents("target");
    writeFixture(source.displayPath(), sourceContents);
    writeFixture(target.displayPath(), targetContents);
    PinnedFile pinned = pin(source);

    PinnedFile copied;
    QString error;
    QVERIFY(!copyToNewFile(pinned, target, copied, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!copied.isValid());
    pinned = {};
    QCOMPARE(readFixture(source.displayPath()), sourceContents);
    QCOMPARE(readFixture(target.displayPath()), targetContents);
}

void TestAnchoredFilesystem::copyReportsNonDurableCleanup()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync cleanup reporting is Unix-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    writeFixture(source.displayPath(), QByteArray("source"));
    const PinnedFile pinned = pin(source);

    failDirectorySync = true;
    PinnedFile copied;
    QString error;
    const bool succeeded = copyToNewFile(
        pinned, target, copied, error);
    failDirectorySync = false;

    QVERIFY(!succeeded);
    QVERIFY(!copied.isValid());
    QVERIFY2(
        error.contains(QStringLiteral(
            "incomplete anchored copy cleanup was not durable")),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(target.displayPath()));
#endif
}

void TestAnchoredFilesystem::replaceExchangesPinnedGenerations()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef staging = entry(directory, QStringLiteral("staging"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QByteArray stagedContents("new activity bytes");
    const QByteArray targetContents("old activity bytes");
    writeFixture(staging.displayPath(), stagedContents);
    writeFixture(target.displayPath(), targetContents);
#ifdef Q_OS_WIN
    QVERIFY(setWindowsCreationTime(
        staging.displayPath(), quint64(132223104000000000ULL)));
    QVERIFY(setWindowsCreationTime(
        target.displayPath(), quint64(133801632000000000ULL)));
#endif
    PinnedFile staged = pin(staging);
    PinnedFile expectedTarget = pin(target);
    const NativeIdentity stagedIdentity = staged.identity();
    const NativeIdentity targetIdentity = expectedTarget.identity();
#ifdef Q_OS_WIN
    const QByteArray stagedGeneration = staged.durableGeneration();
    const QByteArray targetGeneration = expectedTarget.durableGeneration();
    QVERIFY(!stagedGeneration.isEmpty());
    QVERIFY(!targetGeneration.isEmpty());
    QVERIFY(stagedGeneration != targetGeneration);
#endif

    const MutationResult result = replaceExisting(
        staged, expectedTarget);

    verifyApplied(result);
#ifdef Q_OS_WIN
    const NativeIdentity publishedIdentity = staged.identity();
    QVERIFY(publishedIdentity != stagedIdentity);
    QCOMPARE(
        publishedIdentity.serializedKey(),
        stagedIdentity.serializedKey());
    QCOMPARE(staged.durableGeneration(), targetGeneration);
    verifyPinnedAt(staged, target, publishedIdentity);
    QByteArray retainedTarget;
    QString retainedError;
    QVERIFY(readAll(
        expectedTarget, 1024, retainedTarget, retainedError));
    QVERIFY2(retainedError.isEmpty(), qPrintable(retainedError));
    QCOMPARE(retainedTarget, targetContents);
    QCOMPARE(expectedTarget.identity(), targetIdentity);
    QVERIFY(!QFileInfo::exists(staging.displayPath()));
#else
    verifyPinnedAt(staged, target, stagedIdentity);
    verifyPinnedAt(expectedTarget, staging, targetIdentity);
#endif
    QCOMPARE(readFixture(target.displayPath()), stagedContents);
#ifndef Q_OS_WIN
    QCOMPARE(readFixture(staging.displayPath()), targetContents);
#endif
}

void TestAnchoredFilesystem::replaceReportsTargetSubstitutionAtMutation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef staging = entry(directory, QStringLiteral("staging"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray stagedContents("new activity bytes");
    const QByteArray targetContents("old activity bytes");
    writeFixture(staging.displayPath(), stagedContents);
    writeFixture(target.displayPath(), targetContents);
    PinnedFile staged = pin(staging);
    PinnedFile expectedTarget = pin(target);
    PinnedFile substitute;
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &targetPath) {
        if (qstrcmp(transition, "replace-before-publish") != 0) return;
        hookReached = true;
        QVERIFY(renameFixture(targetPath, retained));
        writeFixture(targetPath, targetContents);
        substitute = pin(target);
    };

    const MutationResult result = replaceExisting(
        staged, expectedTarget);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(!result.applied());
#ifdef Q_OS_WIN
    QCOMPARE(result.effect, MutationEffect::Conflict);
    const NativeIdentity substituteIdentity = substitute.identity();
    substitute = {};
    QCOMPARE(pin(target).identity(), substituteIdentity);
    verifyPinnedAt(staged, staging, staged.identity());
#else
    QCOMPARE(result.effect, MutationEffect::Partial);
    const NativeIdentity substituteIdentity = substitute.identity();
    substitute = {};
    QCOMPARE(pin(staging).identity(), substituteIdentity);
    QCOMPARE(pin(target).identity(), staged.identity());
#endif
    verifyPinnedAt(expectedTarget,
                   entry(directory, QStringLiteral("retained")),
                   expectedTarget.identity());
#ifdef Q_OS_WIN
    QCOMPARE(readFixture(target.displayPath()), targetContents);
    QCOMPARE(readFixture(staging.displayPath()), stagedContents);
#else
    QCOMPARE(readFixture(target.displayPath()), stagedContents);
    QCOMPARE(readFixture(staging.displayPath()), targetContents);
#endif
    QCOMPARE(readFixture(retained), targetContents);
}

void TestAnchoredFilesystem::replaceReportsStagingSubstitutionAtMutation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef staging = entry(directory, QStringLiteral("staging"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray stagedContents("new activity bytes");
    const QByteArray targetContents("old activity bytes");
    writeFixture(staging.displayPath(), stagedContents);
    writeFixture(target.displayPath(), targetContents);
    PinnedFile staged = pin(staging);
    PinnedFile expectedTarget = pin(target);
    PinnedFile substitute;
    PinnedFile retainedStaging;
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &stagingPath,
                           const QString &) {
        if (qstrcmp(transition, "replace-before-publish") != 0) return;
        hookReached = true;
        QVERIFY(renameFixture(stagingPath, retained));
        writeFixture(stagingPath, stagedContents);
        substitute = pin(staging);
        retainedStaging = pin(entry(
            directory, QStringLiteral("retained")));
    };

    const MutationResult result = replaceExisting(
        staged, expectedTarget);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(!result.applied());
#ifdef Q_OS_WIN
    QCOMPARE(result.effect, MutationEffect::Conflict);
    const NativeIdentity substituteIdentity = substitute.identity();
    substitute = {};
    QCOMPARE(pin(staging).identity(), substituteIdentity);
    verifyPinnedAt(expectedTarget, target, expectedTarget.identity());
#else
    QCOMPARE(result.effect, MutationEffect::Partial);
    const NativeIdentity substituteIdentity = substitute.identity();
    substitute = {};
    QCOMPARE(pin(target).identity(), substituteIdentity);
    verifyPinnedAt(expectedTarget, staging, expectedTarget.identity());
#endif
    verifyPinnedAt(retainedStaging,
                   entry(directory, QStringLiteral("retained")),
                   retainedStaging.identity());
#ifdef Q_OS_WIN
    QCOMPARE(readFixture(target.displayPath()), targetContents);
    QCOMPARE(readFixture(staging.displayPath()), stagedContents);
#else
    QCOMPARE(readFixture(target.displayPath()), stagedContents);
    QCOMPARE(readFixture(staging.displayPath()), targetContents);
#endif
    QCOMPARE(readFixture(retained), stagedContents);
}

void TestAnchoredFilesystem::replaceDoesNotAttemptUnverifiedRollback()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX atomic name exchange behavior is required");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef staging = entry(directory, QStringLiteral("staging"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray stagedContents("new activity bytes");
    const QByteArray targetContents("old activity bytes");
    const QByteArray substituteContents("foreign activity bytes");
    writeFixture(staging.displayPath(), stagedContents);
    writeFixture(target.displayPath(), targetContents);
    PinnedFile staged = pin(staging);
    PinnedFile expectedTarget = pin(target);
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &targetPath) {
        if (qstrcmp(transition, "replace-published") != 0) return;
        hookReached = true;
        QVERIFY(renameFixture(targetPath, retained));
        writeFixture(targetPath, substituteContents);
    };

    const MutationResult result = replaceExisting(
        staged, expectedTarget);
    filesystemAction = {};

    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readFixture(target.displayPath()), substituteContents);
    QCOMPARE(readFixture(staging.displayPath()), targetContents);
    QCOMPARE(readFixture(retained), stagedContents);
#endif
}

void TestAnchoredFilesystem::moveDoesNotRestoreUnverifiedDestination()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        actionReached = true;
        QVERIFY(renameFixture(destination, retained));
        writeFixture(destination, substitute);
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(result.verifiedRecoveryPath.isEmpty());
    pinned = {};
    QVERIFY(!QFileInfo::exists(source.displayPath()));
    QCOMPARE(readFixture(target.displayPath()), substitute);
    QCOMPARE(readFixture(retained), original);
}

void TestAnchoredFilesystem::moveDoesNotReportModifiedRecoveryPath()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix in-place rewrite race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QByteArray original("original bytes");
    const QByteArray replacement("modified bytes");
    QCOMPARE(replacement.size(), original.size());
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        hookReached = true;
        QFile file(destination);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(0));
        QCOMPARE(file.write(replacement), qint64(replacement.size()));
        QVERIFY(file.flush());
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.applied());
    QVERIFY2(result.verifiedRecoveryPath.isEmpty(),
             "A modified file was advertised as a verified recovery path");
    QCOMPARE(readFixture(target.displayPath()), replacement);
#endif
}

void TestAnchoredFilesystem::moveRejectsReplacementAfterDirectorySync()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory synchronization is Unix-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(
                transition, "move-before-final-name-check") != 0) {
            return;
        }
        hookReached = true;
        QVERIFY(renameFixture(destination, retained));
        writeFixture(destination, substitute);
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.applied());
    QVERIFY(result.verifiedRecoveryPath.isEmpty());
    pinned = {};
    QCOMPARE(readFixture(target.displayPath()), substitute);
    QCOMPARE(readFixture(retained), original);
#endif
}

void TestAnchoredFilesystem::moveRejectsNewHardLink()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString extra = root.filePath(QStringLiteral("extra-link"));
    writeFixture(source.displayPath(), QByteArray("contents"));
    PinnedFile pinned = pin(source);
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        actionReached = true;
        QVERIFY(createHardLink(destination, extra));
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY2(result.verifiedRecoveryPath.isEmpty(),
             "A multiply linked file was advertised as recoverable");
    QVERIFY(QFileInfo::exists(target.displayPath()));
    QVERIFY(QFileInfo::exists(extra));
}

void TestAnchoredFilesystem::moveUsesPinnedParentAfterPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString retainedPath = root.filePath(QStringLiteral("retained-dir"));
    const QString targetPath = root.filePath(QStringLiteral("target-dir"));
    QVERIFY(QDir().mkdir(sourcePath));
    QVERIFY(QDir().mkdir(targetPath));

    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const DirectoryAnchor targetDirectory = openDirectory(targetPath);
    const EntryRef source = entry(sourceDirectory, QStringLiteral("activity"));
    const EntryRef target = entry(targetDirectory, QStringLiteral("moved"));
    const QByteArray contents("same-content identity sentinel");
    writeFixture(source.displayPath(), contents);
    PinnedFile pinned = pin(source);
    const NativeIdentity originalIdentity = pinned.identity();

    const bool replaced = QDir().rename(sourcePath, retainedPath);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(moveNoReplace(pinned, target));
        verifyPinnedAt(pinned, target, originalIdentity);
        return;
    }
    QVERIFY(QDir().mkdir(sourcePath));
    const QString substitutePath =
        QDir(sourcePath).filePath(QStringLiteral("activity"));
    writeFixture(substitutePath, contents);
    const NativeIdentity substituteIdentity =
        pin(entry(openDirectory(sourcePath), QStringLiteral("activity")))
            .identity();

    verifyApplied(moveNoReplace(pinned, target));
    QCOMPARE(readFixture(substitutePath), contents);
    verifyPinnedAt(pinned, target, originalIdentity);
    QVERIFY(originalIdentity != substituteIdentity);
    QVERIFY(!QFileInfo::exists(
        QDir(retainedPath).filePath(QStringLiteral("activity"))));
    QCOMPARE(QDir(sourcePath).entryList(
                 QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList({QStringLiteral("activity")}));
}

void TestAnchoredFilesystem::moveRejectsFinalEntryReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray contents("same bytes");
    writeFixture(source.displayPath(), contents);
    PinnedFile original = pin(source);
    const NativeIdentity originalIdentity = original.identity();

    const bool replaced = QFile::rename(source.displayPath(), retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(moveNoReplace(original, target));
        verifyPinnedAt(original, target, originalIdentity);
        return;
    }
    writeFixture(source.displayPath(), contents);
    const NativeIdentity substituteIdentity = pin(source).identity();

    const MutationResult result = moveNoReplace(original, target);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(pin(source).identity(), substituteIdentity);
    QCOMPARE(pin(entry(directory, QStringLiteral("retained"))).identity(),
             originalIdentity);
    QVERIFY(!QFileInfo::exists(target.displayPath()));
}

void TestAnchoredFilesystem::moveDoesNotReplaceDestination()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    writeFixture(source.displayPath(), QByteArray("source"));
    writeFixture(target.displayPath(), QByteArray("target"));
    PinnedFile pinned = pin(source);
    const NativeIdentity sourceIdentity = pinned.identity();
    const NativeIdentity targetIdentity = pin(target).identity();

    const MutationResult result = moveNoReplace(pinned, target);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    verifyPinnedAt(pinned, source, sourceIdentity);
    QCOMPARE(pin(target).identity(), targetIdentity);
}

void TestAnchoredFilesystem::moveDoesNotReplaceDestinationAcrossDirectories()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString targetPath = root.filePath(QStringLiteral("target-dir"));
    QVERIFY(QDir().mkdir(sourcePath));
    QVERIFY(QDir().mkdir(targetPath));
    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const DirectoryAnchor targetDirectory = openDirectory(targetPath);
    const EntryRef source = entry(
        sourceDirectory, QStringLiteral("source"));
    const EntryRef target = entry(
        targetDirectory, QStringLiteral("target"));
    writeFixture(source.displayPath(), QByteArray("source"));
    writeFixture(target.displayPath(), QByteArray("target"));
    PinnedFile pinned = pin(source);
    const NativeIdentity sourceIdentity = pinned.identity();
    const NativeIdentity targetIdentity = pin(target).identity();

    const MutationResult result = moveNoReplace(pinned, target);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    verifyPinnedAt(pinned, source, sourceIdentity);
    QCOMPARE(pin(target).identity(), targetIdentity);
}

void TestAnchoredFilesystem::removeRejectsPinnedParentPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString retainedPath = root.filePath(QStringLiteral("retained-dir"));
    QVERIFY(QDir().mkdir(sourcePath));

    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const EntryRef source = entry(sourceDirectory, QStringLiteral("activity"));
    const QByteArray contents("same-content identity sentinel");
    writeFixture(source.displayPath(), contents);
    PinnedFile pinned = pin(source);

    const bool replaced = QDir().rename(sourcePath, retainedPath);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(remove(pinned));
        QVERIFY(!QFileInfo::exists(
            QDir(sourcePath).filePath(QStringLiteral("activity"))));
        return;
    }
    QVERIFY(QDir().mkdir(sourcePath));
    const QString substitutePath =
        QDir(sourcePath).filePath(QStringLiteral("activity"));
    writeFixture(substitutePath, contents);
    const NativeIdentity substituteIdentity =
        pin(entry(openDirectory(sourcePath), QStringLiteral("activity")))
            .identity();

    const NativeIdentity originalIdentity = pinned.identity();
    const MutationResult result = remove(pinned);
    QCOMPARE(result.effect, MutationEffect::NoEffect);
    QVERIFY(!result.applied());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(pinned.identity(), originalIdentity);
    QCOMPARE(readFixture(substitutePath), contents);
    QCOMPARE(pin(entry(openDirectory(sourcePath),
                       QStringLiteral("activity"))).identity(),
             substituteIdentity);
    const QString retainedActivity =
        QDir(retainedPath).filePath(QStringLiteral("activity"));
    QCOMPARE(readFixture(retainedActivity), contents);
    QCOMPARE(
        pin(entry(openDirectory(retainedPath), QStringLiteral("activity")))
            .identity(),
        originalIdentity);
}

void TestAnchoredFilesystem::removeRejectsFinalEntryReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef retained = entry(directory, QStringLiteral("retained"));
    const QByteArray contents("same bytes");
    writeFixture(source.displayPath(), contents);
    PinnedFile original = pin(source);
    const NativeIdentity originalIdentity = original.identity();

    const bool replaced = QFile::rename(
        source.displayPath(), retained.displayPath());
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(remove(original));
        QVERIFY(!QFileInfo::exists(source.displayPath()));
        return;
    }
    writeFixture(source.displayPath(), contents);
    const NativeIdentity substituteIdentity = pin(source).identity();

    const MutationResult result = remove(original);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    QCOMPARE(pin(source).identity(), substituteIdentity);
    QCOMPARE(pin(retained).identity(), originalIdentity);
}

void TestAnchoredFilesystem::hardensPrivateDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("private"));
#ifdef Q_OS_WIN
    QVERIFY(createCurrentUserOwnedDirectory(path, FILE_ALL_ACCESS));
    QVERIFY(makeWindowsDirectoryPermissive(path));
#else
    QVERIFY(QDir().mkdir(path));
#endif
#ifdef Q_OS_UNIX
    QVERIFY(QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner | QFileDevice::ReadGroup
            | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther
            | QFileDevice::ExeOther));
#endif
#ifdef Q_OS_MACOS
    QVERIFY(installCurrentUserExtendedAcl(path));
#endif
    DirectoryAnchor directory = openDirectory(path);
    QString error;

    QVERIFY2(hardenPrivateDirectory(directory, error), qPrintable(error));
    QVERIFY2(directory.pathMatches(error), qPrintable(error));
#ifdef Q_OS_UNIX
    struct stat status {};
    QCOMPARE(
        ::stat(QFile::encodeName(path).constData(), &status), 0);
    QCOMPARE(status.st_uid, ::geteuid());
    QCOMPARE(status.st_mode & 0777, mode_t(0700));
#ifdef Q_OS_MACOS
    QVERIFY(extendedAclIsAbsent(path));
#endif
#elif defined(Q_OS_WIN)
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(path));
#endif
}

void TestAnchoredFilesystem::
    hardensCurrentUserOwnedDirectoryWithoutWriteOwner()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ownership semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("private"));
    QVERIFY(createCurrentUserOwnedDirectory(
        path, FILE_ALL_ACCESS & ~WRITE_OWNER));
    DirectoryAnchor directory = openDirectory(path);
    QString error;

    QVERIFY2(hardenPrivateDirectory(directory, error), qPrintable(error));
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(path));
#endif
}

#ifdef Q_OS_WIN
void TestAnchoredFilesystem::repeatedPrivateHardeningPreservesPinnedChild()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DirectoryAnchor directory = openPrivateDirectory(root.path());
    const EntryRef child = entry(directory, QStringLiteral("manifest.json"));
    writeFixture(child.displayPath(), QByteArray("immutable manifest"));
    PinnedFile pinned = pin(child);
    QString error;
    bool matches = false;

    QVERIFY2(entryMatches(child, pinned, matches, error), qPrintable(error));
    QVERIFY(matches);
    QVERIFY2(hardenPrivateDirectory(directory, error), qPrintable(error));
    matches = false;
    QVERIFY2(entryMatches(child, pinned, matches, error), qPrintable(error));
    QVERIFY(matches);
}
#endif

void TestAnchoredFilesystem::hardensPrivateDirectoryAcls_data()
{
    QTest::addColumn<QByteArray>("attribute");
    QTest::newRow("access")
        << QByteArray("system.posix_acl_access");
    QTest::newRow("default")
        << QByteArray("system.posix_acl_default");
}

void TestAnchoredFilesystem::hardensPrivateDirectoryAcls()
{
#ifndef Q_OS_LINUX
    QSKIP("Linux POSIX ACL xattrs are platform-specific");
#else
    QFETCH(QByteArray, attribute);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("private"));
    QVERIFY(QDir().mkdir(path));
    int aclError = 0;
    if (!installLinuxAclXattr(path, attribute, aclError)
        && (aclError == ENOTSUP || aclError == EOPNOTSUPP)) {
        QSKIP("The test filesystem does not support POSIX ACLs");
    }
    QCOMPARE(aclError, 0);
    QVERIFY(linuxAclXattrPresent(path, attribute));

    DirectoryAnchor directory = openDirectory(path);
    QString error;
    QVERIFY2(hardenPrivateDirectory(directory, error), qPrintable(error));
    QVERIFY2(directory.pathMatches(error), qPrintable(error));
    QVERIFY(!linuxAclXattrPresent(path, attribute));

    struct stat status {};
    QCOMPARE(
        ::stat(QFile::encodeName(path).constData(), &status), 0);
    QCOMPARE(status.st_uid, ::geteuid());
    QCOMPARE(status.st_mode & 0777, mode_t(0700));
#endif
}

void TestAnchoredFilesystem::validatesCurrentUserControlledParent()
{
#if !defined(Q_OS_UNIX) && !defined(Q_OS_WIN)
    QSKIP("Anchored directory ownership is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
#ifdef Q_OS_UNIX
    QVERIFY(::chmod(
        QFile::encodeName(root.path()).constData(), 0755) == 0);
#elif defined(Q_OS_WIN)
    QVERIFY(makeWindowsDirectoryPermissive(root.path()));
#endif
    const DirectoryAnchor parent = openDirectory(root.path());
    QString error;

    QVERIFY2(
        validateCurrentUserOwnedDirectory(parent, error),
        qPrintable(error));
    QVERIFY2(
        validateCurrentUserControlledDirectory(parent, error),
        qPrintable(error));
#ifdef Q_OS_WIN
    QVERIFY(!windowsDirectoryHasOwnerOnlyAcl(root.path()));
#endif
#endif
}

void TestAnchoredFilesystem::rejectsWritableCurrentUserOwnedParent()
{
#if !defined(Q_OS_UNIX) && !defined(Q_OS_WIN)
    QSKIP("Anchored directory ownership is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
#ifdef Q_OS_UNIX
    QVERIFY(::chmod(
        QFile::encodeName(root.path()).constData(), 0777) == 0);
#elif defined(Q_OS_WIN)
    QVERIFY(makeWindowsDirectoryEveryoneWritable(root.path()));
#endif
    const DirectoryAnchor parent = openDirectory(root.path());
    QString error;
    QVERIFY2(
        validateCurrentUserOwnedDirectory(parent, error),
        qPrintable(error));
    QVERIFY(!validateCurrentUserControlledDirectory(parent, error));
    QVERIFY(!error.isEmpty());

    DirectoryAnchor child;
    const MutationResult result = createPrivateFixedChildDirectory(
        parent, QStringLiteral("journal"), child);
    QCOMPARE(result.effect, MutationEffect::NoEffect);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("journal"))));
#endif
}

void TestAnchoredFilesystem::acceptsPrivateGroupWritableParent()
{
#ifndef Q_OS_LINUX
    QSKIP("Private user-group validation is Linux-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(::chmod(
        QFile::encodeName(root.path()).constData(), 0775) == 0);
    const QFileInfo rootInfo(root.path());
    if (rootInfo.owner().isEmpty()
        || rootInfo.owner() != rootInfo.group()) {
        QSKIP("The test account does not use a user-private group");
    }
    const DirectoryAnchor parent = openDirectory(root.path());
    QString error;
    QVERIFY2(
        validateCurrentUserControlledDirectory(parent, error),
        qPrintable(error));

    DirectoryAnchor child;
    const MutationResult result = createPrivateFixedChildDirectory(
        parent, QStringLiteral("journal"), child);
    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(child.isValid());
    struct stat parentStatus {};
    struct stat childStatus {};
    QCOMPARE(
        ::stat(QFile::encodeName(root.path()).constData(), &parentStatus),
        0);
    QCOMPARE(parentStatus.st_mode & 0777, mode_t(0775));
    QCOMPARE(
        ::stat(QFile::encodeName(
                   root.filePath(QStringLiteral("journal"))).constData(),
               &childStatus),
        0);
    QCOMPARE(childStatus.st_mode & 0777, mode_t(0700));
#endif
}

void TestAnchoredFilesystem::createsPrivateChildDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(child.isValid());
    QString error;
    QVERIFY2(child.pathMatches(error), qPrintable(error));
    DirectoryAnchor reopened;
    QVERIFY2(
        parent.openChild(QStringLiteral("journal"), reopened, error),
        qPrintable(error));
    QCOMPARE(reopened.identity(), child.identity());
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList {QStringLiteral("journal")});
#ifdef Q_OS_UNIX
    struct stat status {};
    QCOMPARE(
        ::stat(QFile::encodeName(
                   root.filePath(QStringLiteral("journal"))).constData(),
               &status),
        0);
    QCOMPARE(status.st_uid, ::geteuid());
    QCOMPARE(status.st_mode & 0777, mode_t(0700));
#elif defined(Q_OS_WIN)
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(
        root.filePath(QStringLiteral("journal"))));
#endif
}

void TestAnchoredFilesystem::
createsPrivateFixedChildInPermissiveParent()
{
#if !defined(Q_OS_UNIX) && !defined(Q_OS_WIN)
    QSKIP("Private fixed child directories are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeOwnedDirectoryPermissive(root.path()));
#ifdef Q_OS_WIN
    QVERIFY(!windowsDirectoryHasOwnerOnlyAcl(root.path()));
#endif
    const DirectoryAnchor parent = openDirectory(root.path());
    QString error;
    QVERIFY2(
        validateCurrentUserControlledDirectory(parent, error),
        qPrintable(error));
    DirectoryAnchor child;

    const MutationResult result = createPrivateFixedChildDirectory(
        parent, QStringLiteral("journal"), child);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(child.isValid());
    QVERIFY2(child.pathMatches(error), qPrintable(error));
    DirectoryAnchor reopened;
    QVERIFY2(
        parent.openChild(QStringLiteral("journal"), reopened, error),
        qPrintable(error));
    QCOMPARE(reopened.identity(), child.identity());
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList {QStringLiteral("journal")});

#ifdef Q_OS_UNIX
    struct stat parentStatus {};
    struct stat childStatus {};
    QCOMPARE(
        ::stat(QFile::encodeName(root.path()).constData(),
               &parentStatus),
        0);
    QCOMPARE(parentStatus.st_uid, ::geteuid());
    QCOMPARE(parentStatus.st_mode & 0777, mode_t(0755));
    QCOMPARE(
        ::stat(QFile::encodeName(
                   root.filePath(QStringLiteral("journal"))).constData(),
               &childStatus),
        0);
    QCOMPARE(childStatus.st_uid, ::geteuid());
    QCOMPARE(childStatus.st_mode & 0777, mode_t(0700));
#if defined(Q_OS_LINUX)
    QVERIFY(!linuxAclXattrPresent(
        root.filePath(QStringLiteral("journal")),
        QByteArray("system.posix_acl_access")));
    QVERIFY(!linuxAclXattrPresent(
        root.filePath(QStringLiteral("journal")),
        QByteArray("system.posix_acl_default")));
#elif defined(Q_OS_MACOS)
    QVERIFY(extendedAclIsAbsent(
        root.filePath(QStringLiteral("journal"))));
#endif
#elif defined(Q_OS_WIN)
    QVERIFY(!windowsDirectoryHasOwnerOnlyAcl(root.path()));
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(
        root.filePath(QStringLiteral("journal"))));
#endif
#endif
}

void TestAnchoredFilesystem::
strictPrivateChildRejectsPermissiveParent()
{
#if !defined(Q_OS_UNIX) && !defined(Q_OS_WIN)
    QSKIP("Private child directories are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeOwnedDirectoryPermissive(root.path()));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);

    QCOMPARE(result.effect, MutationEffect::NoEffect);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("journal"))));
#ifdef Q_OS_UNIX
    struct stat parentStatus {};
    QCOMPARE(
        ::stat(QFile::encodeName(root.path()).constData(),
               &parentStatus),
        0);
    QCOMPARE(parentStatus.st_mode & 0777, mode_t(0755));
#elif defined(Q_OS_WIN)
    QVERIFY(!windowsDirectoryHasOwnerOnlyAcl(root.path()));
#endif
#endif
}

void TestAnchoredFilesystem::privateFixedChildRejectsParentAcl()
{
#if defined(Q_OS_LINUX)
    const QList<QByteArray> attributes = {
        QByteArray("system.posix_acl_access"),
        QByteArray("system.posix_acl_default")};
    for (const QByteArray &attribute : attributes) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(makeOwnedDirectoryPermissive(root.path()));
        int aclError = 0;
        if (!installLinuxAclXattr(root.path(), attribute, aclError)
            && (aclError == ENOTSUP || aclError == EOPNOTSUPP)) {
            QSKIP("The test filesystem does not support POSIX ACLs");
        }
        QCOMPARE(aclError, 0);
        QVERIFY(linuxAclXattrPresent(root.path(), attribute));
        const DirectoryAnchor parent = openDirectory(root.path());
        DirectoryAnchor child;

        const MutationResult result = createPrivateFixedChildDirectory(
            parent, QStringLiteral("journal"), child);

        QCOMPARE(result.effect, MutationEffect::NoEffect);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!child.isValid());
        QVERIFY(!QFileInfo::exists(
            root.filePath(QStringLiteral("journal"))));
        QVERIFY(linuxAclXattrPresent(root.path(), attribute));
    }
#elif defined(Q_OS_MACOS)
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeOwnedDirectoryPermissive(root.path()));
    QVERIFY(installCurrentUserExtendedAcl(root.path()));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor child;

    const MutationResult result = createPrivateFixedChildDirectory(
        parent, QStringLiteral("journal"), child);

    QCOMPARE(result.effect, MutationEffect::NoEffect);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("journal"))));
    QVERIFY(!extendedAclIsAbsent(root.path()));
#else
    QSKIP("Parent ACL inheritance is Unix platform-specific");
#endif
}

void TestAnchoredFilesystem::privateFixedChildRejectsCollision()
{
#if !defined(Q_OS_UNIX) && !defined(Q_OS_WIN)
    QSKIP("Private fixed child directories are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeOwnedDirectoryPermissive(root.path()));
    const QString existing = root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(existing));
    const QString sentinel = QDir(existing).filePath(
        QStringLiteral("sentinel"));
    writeFixture(sentinel, QByteArray("existing contents"));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor child;

    const MutationResult result = createPrivateFixedChildDirectory(
        parent, QStringLiteral("journal"), child);

    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QCOMPARE(readFixture(sentinel), QByteArray("existing contents"));
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList {QStringLiteral("journal")});
#endif
}

void TestAnchoredFilesystem::
privateFixedChildReportsSyncFailure_data()
{
    QTest::addColumn<QByteArray>("phase");
    QTest::addColumn<int>("expectedEffect");
    QTest::newRow("child")
        << QByteArray("private-fixed-directory-before-child-sync")
        << int(MutationEffect::AppliedNotDurable);
    QTest::newRow("parent")
        << QByteArray("private-fixed-directory-child-synced")
        << int(MutationEffect::AppliedNotDurable);
}

void TestAnchoredFilesystem::
privateFixedChildReportsSyncFailure()
{
#if !defined(Q_OS_UNIX) && !defined(Q_OS_WIN)
    QSKIP("Private fixed child directories are platform-specific");
#else
    QFETCH(QByteArray, phase);
    QFETCH(int, expectedEffect);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeOwnedDirectoryPermissive(root.path()));
    const DirectoryAnchor parent = openDirectory(root.path());
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &, const QString &) {
        if (QByteArray(transition) != phase) return;
        hookReached = true;
        failDirectorySync = true;
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateFixedChildDirectory(
        parent, QStringLiteral("journal"), child);
    filesystemAction = {};
    failDirectorySync = false;

    QVERIFY(hookReached);
    QCOMPARE(result.effect,
             static_cast<MutationEffect>(expectedEffect));
    QVERIFY(result.applied());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(child.isValid());
    QString error;
    QVERIFY2(child.pathMatches(error), qPrintable(error));
#ifdef Q_OS_UNIX
    struct stat childStatus {};
    QCOMPARE(
        ::stat(QFile::encodeName(
                   root.filePath(QStringLiteral("journal"))).constData(),
               &childStatus),
        0);
    QCOMPARE(childStatus.st_uid, ::geteuid());
    QCOMPARE(childStatus.st_mode & 0777, mode_t(0700));
#elif defined(Q_OS_WIN)
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(
        root.filePath(QStringLiteral("journal"))));
#endif
    verifyApplied(removeEmptyDirectory(child));
#endif
}

void TestAnchoredFilesystem::privateChildCreationRejectsCollision()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString existing = root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(existing));
    const QString sentinel = QDir(existing).filePath(
        QStringLiteral("sentinel"));
    writeFixture(sentinel, QByteArray("existing contents"));
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);

    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QCOMPARE(readFixture(sentinel), QByteArray("existing contents"));
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList {QStringLiteral("journal")});
}

void TestAnchoredFilesystem::
privateChildCreationRejectsPublishCollision()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString sentinelPath = QDir(journalPath).filePath(
        QStringLiteral("sentinel"));
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &, const QString &) {
        if (QByteArray(transition)
            != QByteArray("private-directory-before-publish")) {
            return;
        }
        hookReached = true;
        QVERIFY(QDir().mkdir(journalPath));
        writeFixture(sentinelPath, QByteArray("competitor"));
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);
    filesystemAction = {};

    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QCOMPARE(readFixture(sentinelPath), QByteArray("competitor"));
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList {QStringLiteral("journal")});
}

void TestAnchoredFilesystem::
privateChildCreationRejectsReplacedParent()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString namespacePath =
        root.filePath(QStringLiteral("namespace"));
    const QString retainedPath =
        root.filePath(QStringLiteral("namespace.retained"));
#ifdef Q_OS_WIN
    QVERIFY(createCurrentUserOwnedDirectory(
        namespacePath, FILE_ALL_ACCESS));
#else
    QVERIFY(QDir().mkdir(namespacePath));
#endif
    QVERIFY(QFile::setPermissions(
        namespacePath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    const DirectoryAnchor parent = openPrivateDirectory(namespacePath);
    bool hookReached = false;
    bool parentReplaced = false;
    filesystemAction = [&](const char *transition,
                           const QString &, const QString &) {
        if (QByteArray(transition)
            != QByteArray("private-directory-before-publish")) {
            return;
        }
        hookReached = true;
        parentReplaced = QDir().rename(namespacePath, retainedPath);
        if (parentReplaced) QVERIFY(QDir().mkdir(namespacePath));
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);
    filesystemAction = {};

    QVERIFY(hookReached);
    if (!parentReplaced) {
#ifdef Q_OS_WIN
        QCOMPARE(result.effect, MutationEffect::AppliedDurable);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        verifyApplied(removeEmptyDirectory(child));
        return;
#else
        QFAIL("The parent replacement injection did not run");
#endif
    }
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QVERIFY(!QFileInfo::exists(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));
    QVERIFY(QFileInfo(retainedPath).isDir());
}

void TestAnchoredFilesystem::
privateChildCreationRejectsStagingReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    const QString finalPath = root.filePath(QStringLiteral("journal"));
    const QString retainedPath = root.filePath(
        QStringLiteral("retained-staging"));
    QString stagingPath;
    QString sentinelPath;
    bool hookReached = false;
    bool stagingReplaced = false;
    bool sentinelWritten = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (QByteArray(transition)
            != QByteArray("private-directory-staging-anchored")) {
            return;
        }
        hookReached = true;
        stagingPath = primary;
        stagingReplaced = QDir().rename(
            stagingPath, retainedPath);
        if (!stagingReplaced || !QDir().mkdir(stagingPath)) return;
        sentinelPath = QDir(stagingPath).filePath(
            QStringLiteral("sentinel"));
        writeFixture(sentinelPath, QByteArray("substitute"));
        sentinelWritten = true;
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);

    QVERIFY(hookReached);
    if (!stagingReplaced) {
#ifdef Q_OS_WIN
        verifyApplied(removeEmptyDirectory(child));
        QSKIP("The anchored Windows staging handle blocks replacement");
#else
        QFAIL("The staging replacement injection did not run");
#endif
    }
    QVERIFY(sentinelWritten);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!child.isValid());
    QVERIFY(!QFileInfo::exists(finalPath));
    QCOMPARE(readFixture(sentinelPath), QByteArray("substitute"));
    QVERIFY(QFileInfo(retainedPath).isDir());
}

void TestAnchoredFilesystem::
privateChildCreationRetainsNonEmptyStaging()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    const QString finalPath = root.filePath(QStringLiteral("journal"));
    const QString competitorPath = QDir(finalPath).filePath(
        QStringLiteral("competitor"));
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &secondary) {
        if (QByteArray(transition)
            != QByteArray("private-directory-before-publish")) {
            return;
        }
        hookReached = true;
        writeFixture(
            QDir(primary).filePath(QStringLiteral("recovery-data")),
            QByteArray("retained"));
        QVERIFY(QDir().mkdir(secondary));
        writeFixture(competitorPath, QByteArray("competitor"));
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);

    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!result.verifiedRecoveryPath.isEmpty());
    QVERIFY(!child.isValid());
    QCOMPARE(readFixture(competitorPath), QByteArray("competitor"));
    QCOMPARE(
        readFixture(
            QDir(result.verifiedRecoveryPath).filePath(
                QStringLiteral("recovery-data"))),
        QByteArray("retained"));
}

void TestAnchoredFilesystem::
privateChildCreationRejectsPublishedReplacement_data()
{
    QTest::addColumn<QByteArray>("phase");
    QTest::newRow("published")
        << QByteArray("private-directory-published");
    QTest::newRow("final-name-check")
        << QByteArray("private-directory-before-final-name-check");
}

void TestAnchoredFilesystem::
privateChildCreationRejectsPublishedReplacement()
{
    QFETCH(QByteArray, phase);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString retainedPath =
        root.filePath(QStringLiteral("journal.retained"));
    const QString sentinelPath = QDir(journalPath).filePath(
        QStringLiteral("sentinel"));
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    bool hookReached = false;
    bool childReplaced = false;
    bool sentinelWritten = false;
    filesystemAction = [&](const char *transition,
                           const QString &, const QString &) {
        if (QByteArray(transition) != phase) {
            return;
        }
        hookReached = true;
        childReplaced = QDir().rename(journalPath, retainedPath);
        if (!childReplaced || !QDir().mkdir(journalPath)) return;
        QFile sentinel(sentinelPath);
        sentinelWritten = sentinel.open(
                QIODevice::WriteOnly | QIODevice::Truncate)
            && sentinel.write("replacement") == 11
            && sentinel.flush();
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);
    filesystemAction = {};

    QVERIFY(hookReached);
    if (!childReplaced) {
#ifdef Q_OS_WIN
        QCOMPARE(result.effect, MutationEffect::AppliedDurable);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        verifyApplied(removeEmptyDirectory(child));
        return;
#else
        QFAIL("The child replacement injection did not run");
#endif
    }
    QVERIFY(sentinelWritten);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(child.isValid());
    QCOMPARE(readFixture(sentinelPath), QByteArray("replacement"));
    QVERIFY(QFileInfo(retainedPath).isDir());
}

void TestAnchoredFilesystem::
privateChildCreationReportsParentSyncFailure()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory durability is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor parent = openPrivateDirectory(root.path());
    bool hookReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &, const QString &) {
        if (QByteArray(transition)
            == QByteArray("private-directory-published")) {
            hookReached = true;
            failDirectorySync = true;
        }
    };
    DirectoryAnchor child;

    const MutationResult result = createPrivateChildDirectory(
        parent, QStringLiteral("journal"), child);
    filesystemAction = {};
    failDirectorySync = false;

    QVERIFY(hookReached);
    QCOMPARE(result.effect, MutationEffect::AppliedNotDurable);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(child.isValid());
    QString error;
    QVERIFY2(child.pathMatches(error), qPrintable(error));
    verifyApplied(removeEmptyDirectory(child));
#endif
}

void TestAnchoredFilesystem::removesAnchoredEmptyDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("journal")));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    const MutationResult result = removeEmptyDirectory(journal);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!journal.isValid());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("journal"))));
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList());
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsReplacedParent()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString namespacePath =
        root.filePath(QStringLiteral("namespace"));
    const QString displacedPath =
        root.filePath(QStringLiteral("namespace.displaced"));
    QVERIFY(QDir().mkpath(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));

    const DirectoryAnchor rootAnchor = openDirectory(root.path());
    DirectoryAnchor namespaceAnchor;
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        rootAnchor.openChild(
            QStringLiteral("namespace"), namespaceAnchor, error),
        qPrintable(error));
    QVERIFY2(
        namespaceAnchor.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    const bool replaced = QDir().rename(
        namespacePath, displacedPath);
#ifdef Q_OS_WIN
    if (!replaced) {
        QSKIP("Open Windows directory handles prevent this parent race");
    }
#else
    QVERIFY(replaced);
#endif
    QVERIFY(QDir().mkpath(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));

    const MutationResult result = removeEmptyDirectory(journal);

    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(QFileInfo::exists(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));
    QVERIFY(QFileInfo::exists(
        QDir(displacedPath).filePath(QStringLiteral("journal"))));
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetainsPreQuarantineReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory quarantine is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString retainedPath =
        root.filePath(QStringLiteral("journal.retained"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    NativeIdentity substituteIdentity;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-before-quarantine") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QDir().rename(target, retainedPath));
        QVERIFY(QDir().mkdir(target));
        DirectoryAnchor substitute;
        QString actionError;
        QVERIFY2(
            parent.openChild(
                QStringLiteral("journal"), substitute, actionError),
            qPrintable(actionError));
        substituteIdentity = substitute.identity();
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(QFileInfo(retainedPath).isDir());
    QVERIFY(substituteIdentity.isValid());
    const QFileInfoList directories = QDir(root.path()).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    QCOMPARE(directories.size(), 2);
    for (const QFileInfo &entryInfo : directories) {
        if (entryInfo.absoluteFilePath() == retainedPath) continue;
        DirectoryAnchor retainedSubstitute;
        QVERIFY2(
            parent.openChild(
                entryInfo.fileName(), retainedSubstitute, error),
            qPrintable(error));
        QCOMPARE(retainedSubstitute.identity(), substituteIdentity);
    }
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetainsFinalNameReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory unlink race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString retainedPath =
        root.filePath(QStringLiteral("journal.retained"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    QString replacementPath;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        actionReached = true;
        replacementPath = target;
        QVERIFY(QDir().rename(target, retainedPath));
        QVERIFY(QDir().mkdir(target));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(QFileInfo(retainedPath).isDir());
    QVERIFY2(
        QFileInfo(replacementPath).isDir(),
        "Directory removal deleted a substituted final component");
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetainsNonEmptyQuarantine()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory quarantine is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    QString quarantinePath;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        quarantinePath = target;
        writeFixture(
            QDir(target).filePath(QStringLiteral("entry")),
            QByteArray("entry"));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(!quarantinePath.isEmpty());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(journal.isValid());
    QCOMPARE(result.verifiedRecoveryPath, quarantinePath);
    QVERIFY(QFileInfo(QDir(quarantinePath).filePath(
        QStringLiteral("entry"))).isFile());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsRepopulatedOriginalName()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory quarantine is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QDir().mkdir(journalPath));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!journal.isValid());
    QVERIFY(QFileInfo(journalPath).isDir());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalReportsPartialSyncFailure()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory durability is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        writeFixture(
            QDir(target).filePath(QStringLiteral("entry")),
            QByteArray("entry"));
    };
    failDirectorySync = true;

    const MutationResult result = removeEmptyDirectory(journal);
    failDirectorySync = false;
    filesystemAction = {};

    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(result.error.contains(QStringLiteral("synchronization")));
    QVERIFY(journal.isValid());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalReportsParentSyncFailure()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory durability is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    failDirectorySync = true;
    const MutationResult result = removeEmptyDirectory(journal);
    failDirectorySync = false;

    QCOMPARE(result.effect, MutationEffect::AppliedNotDurable);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!journal.isValid());
    QVERIFY(!QFileInfo::exists(journalPath));
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalCanRetryAfterNonEmptyFailure()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString childPath =
        QDir(journalPath).filePath(QStringLiteral("entry"));
    QVERIFY(QDir().mkdir(journalPath));
    writeFixture(childPath, QByteArray("entry"));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    const MutationResult first = removeEmptyDirectory(journal);

    QCOMPARE(first.effect, MutationEffect::NoEffect);
    QVERIFY(!first.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(QFile::remove(childPath));

    const MutationResult retry = removeEmptyDirectory(journal);

    QCOMPARE(retry.effect, MutationEffect::AppliedDurable);
    QVERIFY2(retry.error.isEmpty(), qPrintable(retry.error));
    QVERIFY(!journal.isValid());
    QVERIFY(!QFileInfo::exists(journalPath));
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetriesAfterDeleteSharingConflict()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    {
        WindowsTestHandle observer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(journalPath.utf16()),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        QVERIFY(observer.isValid());

        const MutationResult first = removeEmptyDirectory(journal);

        QCOMPARE(first.effect, MutationEffect::NoEffect);
        QVERIFY(!first.error.isEmpty());
        QVERIFY(journal.isValid());
        error.clear();
        QVERIFY2(journal.pathMatches(error), qPrintable(error));
    }

    const MutationResult retry = removeEmptyDirectory(journal);

    QCOMPARE(retry.effect, MutationEffect::AppliedDurable);
    QVERIFY2(retry.error.isEmpty(), qPrintable(retry.error));
    QVERIFY(!journal.isValid());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsWindowsRepopulation()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory removal is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-disposition-completed") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QDir().mkdir(target));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!journal.isValid());
    QVERIFY(QFileInfo(journalPath).isDir());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsWindowsAliases()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));
    DirectoryAnchor alias = journal;

    const MutationResult aliased = removeEmptyDirectory(journal);

    QCOMPARE(aliased.effect, MutationEffect::NoEffect);
    QVERIFY(!aliased.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(alias.isValid());
    error.clear();
    QVERIFY2(journal.pathMatches(error), qPrintable(error));
    error.clear();
    QVERIFY2(alias.pathMatches(error), qPrintable(error));

    alias = {};
    const MutationResult retry = removeEmptyDirectory(journal);

    QCOMPARE(retry.effect, MutationEffect::AppliedDurable);
    QVERIFY(!journal.isValid());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalUnlinksWindowsNameWithSharedObserver()
{
#ifndef Q_OS_WIN
    QSKIP("Windows shared-delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));
    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(journalPath.utf16()),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());

    const MutationResult result = removeEmptyDirectory(journal);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!journal.isValid());
    QVERIFY(QDir().mkdir(journalPath));
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalLegacyWindowsDeleteReportsPendingName()
{
#ifndef Q_OS_WIN
    QSKIP("Windows legacy delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    {
        WindowsTestHandle observer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(journalPath.utf16()),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        QVERIFY(observer.isValid());

        forceLegacyWindowsDelete = true;
        const MutationResult result = removeEmptyDirectory(journal);
        forceLegacyWindowsDelete = false;

        QCOMPARE(result.effect, MutationEffect::Partial);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!journal.isValid());
        QVERIFY(!QDir().mkdir(journalPath));
    }

    QVERIFY(QDir().mkdir(journalPath));
#endif
}

void TestAnchoredFilesystem::removeRetainsReplacementAtQuarantine()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix unlink race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &quarantine,
                           const QString &) {
        if (qstrcmp(transition, "remove-quarantine-verified") != 0) return;
        actionReached = true;
        QVERIFY(QFile::rename(quarantine, retained));
        writeFixture(quarantine, substitute);
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QCOMPARE(readFixture(retained), original);
    const QStringList entries = QDir(root.path()).entryList(
        QStringList({QStringLiteral(".gc-remove-*")}),
        QDir::Files | QDir::Hidden);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(readFixture(root.filePath(entries.constFirst())), substitute);
#endif
}

void TestAnchoredFilesystem::removePartialMoveReportsGeneratedQuarantine()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix quarantine move is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    QString quarantine;
    QString extraLink;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        quarantine = destination;
        extraLink = destination + QStringLiteral(".extra-link");
        QVERIFY(createHardLink(destination, extraLink));
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};

    QVERIFY(!quarantine.isEmpty());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY2(result.verifiedRecoveryPath.isEmpty(),
             "A multiply linked quarantine was advertised as recoverable");
    QCOMPARE(readFixture(quarantine), original);
    QCOMPARE(readFixture(extraLink), original);
    QVERIFY(!QFileInfo::exists(source.displayPath()));
#endif
}

void TestAnchoredFilesystem::
removeDetectsReplacementAfterFinalQuarantineCheck()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix unlink race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    const NativeIdentity originalIdentity = pinned.identity();
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &quarantine,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-quarantine-finally-verified") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QFile::rename(quarantine, retained));
        writeFixture(quarantine, substitute);
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(result.verifiedRecoveryPath.isEmpty());
    QVERIFY(pinned.isValid());
    QCOMPARE(pinned.identity(), originalIdentity);
    QCOMPARE(readFixture(retained), original);
    const QStringList entries = QDir(root.path()).entryList(
        QStringList({QStringLiteral(".gc-remove-*")}),
        QDir::Files | QDir::Hidden);
    QVERIFY(entries.isEmpty());
#endif
}

void TestAnchoredFilesystem::
removeUnlinksWindowsNameWithSharedObserver()
{
#ifndef Q_OS_WIN
    QSKIP("Windows shared-delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original contents");
    const QByteArray replacement("replacement contents");
    writeFixture(source.displayPath(), original);

    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(source.displayPath().utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());
    PinnedFile pinned = pin(source);

    const MutationResult result = remove(pinned);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY(!pinned.isValid());
    writeFixture(source.displayPath(), replacement);

    LARGE_INTEGER start {};
    QVERIFY(::SetFilePointerEx(
        observer.get(), start, nullptr, FILE_BEGIN));
    QByteArray observed(original.size(), '\0');
    DWORD bytesRead = 0;
    QVERIFY(::ReadFile(
        observer.get(), observed.data(), DWORD(observed.size()),
        &bytesRead, nullptr));
    QCOMPARE(bytesRead, DWORD(original.size()));
    QCOMPARE(observed, original);
    QCOMPARE(readFixture(source.displayPath()), replacement);
#endif
}

void TestAnchoredFilesystem::
removeLegacyWindowsDeleteReportsPendingName()
{
#ifndef Q_OS_WIN
    QSKIP("Windows legacy delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original contents");
    writeFixture(source.displayPath(), original);

    {
        WindowsTestHandle observer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(source.displayPath().utf16()),
            GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        QVERIFY(observer.isValid());
        PinnedFile pinned = pin(source);

        forceLegacyWindowsDelete = true;
        const MutationResult result = remove(pinned);
        forceLegacyWindowsDelete = false;

        QCOMPARE(result.effect, MutationEffect::Partial);
        QVERIFY(!pinned.isValid());
        QVERIFY2(result.verifiedRecoveryPath.isEmpty(),
                 "A path reopened after pin retirement was reported as verified");
        QFile replacement(source.displayPath());
        QVERIFY(!replacement.open(
            QIODevice::WriteOnly | QIODevice::NewOnly));

        LARGE_INTEGER start {};
        QVERIFY(::SetFilePointerEx(
            observer.get(), start, nullptr, FILE_BEGIN));
        QByteArray observed(original.size(), '\0');
        DWORD bytesRead = 0;
        QVERIFY(::ReadFile(
            observer.get(), observed.data(), DWORD(observed.size()),
            &bytesRead, nullptr));
        QCOMPARE(bytesRead, DWORD(original.size()));
        QCOMPARE(observed, original);
    }

    writeFixture(source.displayPath(), QByteArray("replacement"));
#endif
}

void TestAnchoredFilesystem::syncsPinnedDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    QString error;
    QVERIFY(directory.sync(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

QTEST_GUILESS_MAIN(TestAnchoredFilesystem)

#include "testAnchoredFilesystem.moc"
