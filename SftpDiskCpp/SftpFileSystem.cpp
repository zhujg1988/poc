// SftpFileSystem.cpp — WinFSP callback implementations using libssh2 SFTP.

#include "SftpFileSystem.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

// ── string conversion helpers ────────────────────────────────────────────────

static std::string WideToUtf8(PCWSTR wide)
{
    if (!wide || !*wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const char *utf8)
{
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], len);
    return w;
}

// ── SftpFileSystem ───────────────────────────────────────────────────────────

SftpFileSystem::SftpFileSystem(LIBSSH2_SFTP *sftp, const std::string &rootPath)
    : _sftp(sftp)
    , _rootPath(rootPath)
{
    // Strip trailing slashes from root path.
    while (!_rootPath.empty() && _rootPath.back() == '/')
        _rootPath.pop_back();
}

// ── private helpers ──────────────────────────────────────────────────────────

std::string SftpFileSystem::ToRemotePath(PCWSTR windowsPath) const
{
    std::string posix = WideToUtf8(windowsPath);
    for (char &c : posix)
        if (c == '\\') c = '/';
    return _rootPath + posix;
}

/*static*/ void SftpFileSystem::FillFileInfo(FSP_FSCTL_FILE_INFO *fi,
                                              const LIBSSH2_SFTP_ATTRIBUTES &attrs,
                                              bool isDir)
{
    memset(fi, 0, sizeof(*fi));

    // Permissions
    if (!isDir && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
        isDir = (S_ISDIR(attrs.permissions) != 0);

    fi->FileAttributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    // Size
    if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
        fi->FileSize       = attrs.filesize;
        fi->AllocationSize = (attrs.filesize + 4095ULL) & ~4095ULL;
    }

    // Timestamps
    if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
        UINT64 atime = UnixToFileTime(attrs.atime);
        UINT64 mtime = UnixToFileTime(attrs.mtime);
        fi->CreationTime   = mtime; // SFTP has no ctime; fall back to mtime
        fi->LastAccessTime = atime;
        fi->LastWriteTime  = mtime;
        fi->ChangeTime     = mtime;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        UINT64 now = (static_cast<UINT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        fi->CreationTime = fi->LastAccessTime = fi->LastWriteTime = fi->ChangeTime = now;
    }
}

NTSTATUS SftpFileSystem::StatPath(const std::string &path, bool isDir, FSP_FSCTL_FILE_INFO *fi)
{
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    int rc;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        rc = libssh2_sftp_stat(_sftp, path.c_str(), &attrs);
    }
    if (rc != 0)
        return SftpError();
    FillFileInfo(fi, attrs, isDir);
    return STATUS_SUCCESS;
}

NTSTATUS SftpFileSystem::StatHandle(LIBSSH2_SFTP_HANDLE *h, bool isDir, FSP_FSCTL_FILE_INFO *fi)
{
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    int rc;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        rc = libssh2_sftp_fstat(h, &attrs);
    }
    if (rc != 0)
        return SftpError();
    FillFileInfo(fi, attrs, isDir);
    return STATUS_SUCCESS;
}

NTSTATUS SftpFileSystem::SftpError() const
{
    unsigned long err = libssh2_sftp_last_error(_sftp);
    switch (err) {
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case LIBSSH2_FX_PERMISSION_DENIED:
        return STATUS_ACCESS_DENIED;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS:
        return STATUS_OBJECT_NAME_COLLISION;
    case LIBSSH2_FX_DIR_NOT_EMPTY:
        return STATUS_DIRECTORY_NOT_EMPTY;
    case LIBSSH2_FX_OP_UNSUPPORTED:
        return STATUS_NOT_SUPPORTED;
    default:
        return STATUS_UNEXPECTED_IO_ERROR;
    }
}

/*static*/ unsigned long SftpFileSystem::FileTimeToUnix(UINT64 ft)
{
    // FILETIME is 100-ns intervals since 1601-01-01; Unix is seconds since 1970-01-01.
    return static_cast<unsigned long>((ft - 116444736000000000ULL) / 10000000ULL);
}

/*static*/ UINT64 SftpFileSystem::UnixToFileTime(unsigned long ut)
{
    return static_cast<UINT64>(ut) * 10000000ULL + 116444736000000000ULL;
}

// ── GetVolumeInfo ────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::GetVolumeInfo(FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    memset(VolumeInfo, 0, sizeof(*VolumeInfo));

    // Try to get real volume statistics from the server.
    LIBSSH2_SFTP_STATVFS st{};
    bool gotStats = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::string root = _rootPath.empty() ? "/" : _rootPath;
        gotStats = (libssh2_sftp_statvfs(_sftp, root.c_str(),
                                         root.size(), &st) == 0);
    }

    if (gotStats && st.f_frsize > 0) {
        VolumeInfo->TotalSize = st.f_blocks * st.f_frsize;
        VolumeInfo->FreeSize  = st.f_bfree  * st.f_frsize;
    } else {
        // Fallback: synthetic 1 TiB total, half free.
        VolumeInfo->TotalSize = kTotalSize;
        VolumeInfo->FreeSize  = kTotalSize / 2;
    }

    static const WCHAR label[] = L"SFTP";
    VolumeInfo->VolumeLabelLength = sizeof(label) - sizeof(WCHAR);
    memcpy(VolumeInfo->VolumeLabel, label, VolumeInfo->VolumeLabelLength);

    return STATUS_SUCCESS;
}

// ── GetSecurityByName ────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::GetSecurityByName(
    PWSTR FileName,
    PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T *PSecurityDescriptorSize)
{
    std::string path = ToRemotePath(FileName);
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    int rc;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        rc = libssh2_sftp_stat(_sftp, path.c_str(), &attrs);
    }
    if (rc != 0)
        return SftpError();

    bool isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                 ? (S_ISDIR(attrs.permissions) != 0)
                 : false;

    if (PFileAttributes)
        *PFileAttributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    // No security descriptor: return zero size so WinFSP synthesises a permissive one.
    if (PSecurityDescriptorSize)
        *PSecurityDescriptorSize = 0;

    return STATUS_SUCCESS;
}

// ── Create ───────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Create(
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    UINT32 FileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    UINT64 AllocationSize,
    PVOID *PFileNode,
    PVOID *PFileDesc,
    FSP_FSCTL_FILE_INFO *FileInfo,
    PWSTR NormalizedName,
    SIZE_T NormalizedNameSize)
{
    std::string path = ToRemotePath(FileName);
    bool isDir = (CreateOptions & FILE_DIRECTORY_FILE) != 0;
    SftpContext *ctx = nullptr;

    if (isDir) {
        int rc;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            rc = libssh2_sftp_mkdir(_sftp, path.c_str(), 0755);
        }
        if (rc != 0)
            return SftpError();

        ctx = new SftpContext(path, true, nullptr);
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        {
            std::lock_guard<std::mutex> lock(_mutex);
            libssh2_sftp_stat(_sftp, path.c_str(), &attrs);
        }
        FillFileInfo(FileInfo, attrs, true);
    } else {
        LIBSSH2_SFTP_HANDLE *h;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            h = libssh2_sftp_open(_sftp, path.c_str(),
                                  LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE
                                  | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                  LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR
                                  | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
        }
        if (!h)
            return SftpError();

        ctx = new SftpContext(path, false, h);

        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        {
            std::lock_guard<std::mutex> lock(_mutex);
            libssh2_sftp_fstat(h, &attrs);
        }
        FillFileInfo(FileInfo, attrs, false);
    }

    if (NormalizedName && NormalizedNameSize > 0)
        wcsncpy(NormalizedName, FileName,
                NormalizedNameSize / sizeof(WCHAR) - 1);

    *PFileNode = ctx;
    *PFileDesc = ctx;
    return STATUS_SUCCESS;
}

// ── Open ─────────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Open(
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    PVOID *PFileNode,
    PVOID *PFileDesc,
    FSP_FSCTL_FILE_INFO *FileInfo,
    PWSTR NormalizedName,
    SIZE_T NormalizedNameSize)
{
    std::string path = ToRemotePath(FileName);
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (libssh2_sftp_stat(_sftp, path.c_str(), &attrs) != 0)
            return SftpError();
    }

    bool isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                 ? (S_ISDIR(attrs.permissions) != 0)
                 : false;

    SftpContext *ctx = nullptr;

    if (isDir) {
        ctx = new SftpContext(path, true, nullptr);
        FillFileInfo(FileInfo, attrs, true);
    } else {
        // Try read-write; fall back to read-only for write-protected files.
        LIBSSH2_SFTP_HANDLE *h = nullptr;
        bool readOnly = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            h = libssh2_sftp_open(_sftp, path.c_str(),
                                  LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE, 0);
            if (!h) {
                h = libssh2_sftp_open(_sftp, path.c_str(), LIBSSH2_FXF_READ, 0);
                readOnly = true;
            }
        }
        if (!h)
            return SftpError();

        ctx = new SftpContext(path, false, h, readOnly);
        FillFileInfo(FileInfo, attrs, false);
    }

    if (NormalizedName && NormalizedNameSize > 0)
        wcsncpy(NormalizedName, FileName,
                NormalizedNameSize / sizeof(WCHAR) - 1);

    *PFileNode = ctx;
    *PFileDesc = ctx;
    return STATUS_SUCCESS;
}

// ── Overwrite ────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Overwrite(
    PVOID FileNode,
    PVOID FileDesc,
    UINT32 FileAttributes,
    BOOLEAN ReplaceFileAttributes,
    UINT64 AllocationSize,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);

    std::lock_guard<std::mutex> lock(_mutex);
    if (ctx->handle) {
        libssh2_sftp_close(ctx->handle);
        ctx->handle = nullptr;
    }
    ctx->handle = libssh2_sftp_open(_sftp, ctx->remotePathA.c_str(),
                                    LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE
                                    | LIBSSH2_FXF_TRUNC,
                                    0);
    if (!ctx->handle)
        return SftpError();

    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    libssh2_sftp_fstat(ctx->handle, &attrs);
    FillFileInfo(FileInfo, attrs, false);
    return STATUS_SUCCESS;
}

// ── Cleanup ──────────────────────────────────────────────────────────────────

VOID SftpFileSystem::Cleanup(
    PVOID FileNode,
    PVOID FileDesc,
    PWSTR FileName,
    ULONG Flags)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);

    if (Flags & CleanupDelete) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (ctx->handle) {
            libssh2_sftp_close(ctx->handle);
            ctx->handle = nullptr;
        }
        if (ctx->isDirectory)
            libssh2_sftp_rmdir(_sftp, ctx->remotePathA.c_str());
        else
            libssh2_sftp_unlink(_sftp, ctx->remotePathA.c_str());
        // Errors are intentionally swallowed: Cleanup must not throw.
    }
}

// ── Close ────────────────────────────────────────────────────────────────────

VOID SftpFileSystem::Close(PVOID FileNode, PVOID FileDesc)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);
    if (ctx->handle) {
        std::lock_guard<std::mutex> lock(_mutex);
        libssh2_sftp_close(ctx->handle);
        ctx->handle = nullptr;
    }
    delete ctx;
}

// ── Read ─────────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Read(
    PVOID FileNode,
    PVOID FileDesc,
    PVOID Buffer,
    UINT64 Offset,
    ULONG Length,
    PULONG PBytesTransferred)
{
    *PBytesTransferred = 0;
    auto *ctx = static_cast<SftpContext *>(FileDesc);
    if (!ctx->handle)
        return STATUS_INVALID_DEVICE_REQUEST;

    std::lock_guard<std::mutex> lock(_mutex);
    libssh2_sftp_seek64(ctx->handle, Offset);

    ssize_t n = libssh2_sftp_read(ctx->handle, static_cast<char *>(Buffer), Length);
    if (n < 0)
        return SftpError();

    *PBytesTransferred = static_cast<ULONG>(n);
    return STATUS_SUCCESS;
}

// ── Write ────────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Write(
    PVOID FileNode,
    PVOID FileDesc,
    PVOID Buffer,
    UINT64 Offset,
    ULONG Length,
    BOOLEAN WriteToEndOfFile,
    BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    *PBytesTransferred = 0;
    auto *ctx = static_cast<SftpContext *>(FileDesc);
    if (!ctx->handle)
        return STATUS_INVALID_DEVICE_REQUEST;

    std::lock_guard<std::mutex> lock(_mutex);

    if (ConstrainedIo) {
        // Get the current file size.
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        if (libssh2_sftp_fstat(ctx->handle, &attrs) == 0
            && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
        {
            libssh2_uint64_t fileSize = attrs.filesize;
            if (Offset >= fileSize)
                return STATUS_SUCCESS; // write past EOF in constrained mode is a no-op
            if (Offset + Length > fileSize)
                Length = static_cast<ULONG>(fileSize - Offset);
        }
    }

    if (WriteToEndOfFile) {
        // Seek to end: SFTP has no SEEK_END constant; get length from stat.
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        if (libssh2_sftp_fstat(ctx->handle, &attrs) == 0
            && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
        {
            libssh2_sftp_seek64(ctx->handle, attrs.filesize);
        }
    } else {
        libssh2_sftp_seek64(ctx->handle, Offset);
    }

    ssize_t n = libssh2_sftp_write(ctx->handle,
                                    static_cast<const char *>(Buffer), Length);
    if (n < 0)
        return SftpError();

    *PBytesTransferred = static_cast<ULONG>(n);

    // Refresh file info after write.
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    libssh2_sftp_fstat(ctx->handle, &attrs);
    FillFileInfo(FileInfo, attrs, false);

    return STATUS_SUCCESS;
}

// ── Flush ────────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Flush(
    PVOID FileNode,
    PVOID FileDesc,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);

    // libssh2 SFTP has no explicit flush; each write is already sent to the server.
    // Just refresh the file info.
    if (ctx->handle)
        return StatHandle(ctx->handle, ctx->isDirectory, FileInfo);

    return StatPath(ctx->remotePathA, ctx->isDirectory, FileInfo);
}

// ── GetFileInfo ───────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::GetFileInfo(
    PVOID FileNode,
    PVOID FileDesc,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);
    if (ctx->handle)
        return StatHandle(ctx->handle, ctx->isDirectory, FileInfo);
    return StatPath(ctx->remotePathA, ctx->isDirectory, FileInfo);
}

// ── SetBasicInfo ──────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::SetBasicInfo(
    PVOID FileNode,
    PVOID FileDesc,
    UINT32 FileAttributes,
    UINT64 CreationTime,
    UINT64 LastAccessTime,
    UINT64 LastWriteTime,
    UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);

    // Timestamps are best-effort on SFTP; ignore errors.
    if (LastAccessTime != 0 || LastWriteTime != 0) {
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        attrs.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;

        // Read current values first so we can preserve whichever is not being set.
        {
            std::lock_guard<std::mutex> lock(_mutex);
            LIBSSH2_SFTP_ATTRIBUTES cur{};
            if (ctx->handle)
                libssh2_sftp_fstat(ctx->handle, &cur);
            else
                libssh2_sftp_stat(_sftp, ctx->remotePathA.c_str(), &cur);
            if (cur.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
                attrs.atime = cur.atime;
                attrs.mtime = cur.mtime;
            }
            if (LastAccessTime != 0) attrs.atime = FileTimeToUnix(LastAccessTime);
            if (LastWriteTime  != 0) attrs.mtime = FileTimeToUnix(LastWriteTime);

            if (ctx->handle)
                libssh2_sftp_fsetstat(ctx->handle, &attrs);
            else
                libssh2_sftp_setstat(_sftp, ctx->remotePathA.c_str(), &attrs);
        }
    }

    // Return current file info (ignore any attribute-set errors above).
    if (ctx->handle)
        StatHandle(ctx->handle, ctx->isDirectory, FileInfo);
    else
        StatPath(ctx->remotePathA, ctx->isDirectory, FileInfo);

    return STATUS_SUCCESS;
}

// ── SetFileSize ───────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::SetFileSize(
    PVOID FileNode,
    PVOID FileDesc,
    UINT64 NewSize,
    BOOLEAN SetAllocationSize,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);

    if (SetAllocationSize) {
        // Allocation-size changes are advisory; just return current info.
        if (ctx->handle)
            return StatHandle(ctx->handle, false, FileInfo);
        return StatPath(ctx->remotePathA, false, FileInfo);
    }

    if (!ctx->handle)
        return STATUS_INVALID_DEVICE_REQUEST;

    // Use SSH_FXP_FSETSTAT with SSH_FILEXFER_ATTR_SIZE to truncate/extend directly
    // on the server — much more efficient than the C# read-and-rewrite approach.
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    attrs.flags    = LIBSSH2_SFTP_ATTR_SIZE;
    attrs.filesize = static_cast<libssh2_uint64_t>(NewSize);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (libssh2_sftp_fsetstat(ctx->handle, &attrs) != 0)
            return SftpError();
    }

    return StatHandle(ctx->handle, false, FileInfo);
}

// ── CanDelete ────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::CanDelete(PVOID FileNode, PVOID FileDesc, PWSTR FileName)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);
    if (!ctx->isDirectory)
        return STATUS_SUCCESS;

    // Check that the directory is empty while holding the mutex for the whole operation.
    std::lock_guard<std::mutex> lock(_mutex);

    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(_sftp, ctx->remotePathA.c_str());
    if (!dh)
        return SftpError();

    char nameBuf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    bool empty = true;

    for (;;) {
        int rc = libssh2_sftp_readdir(dh, nameBuf, sizeof(nameBuf), &attrs);
        if (rc <= 0) break;
        if (strcmp(nameBuf, ".") != 0 && strcmp(nameBuf, "..") != 0) {
            empty = false;
            break;
        }
    }

    libssh2_sftp_close(dh);
    return empty ? STATUS_SUCCESS : STATUS_DIRECTORY_NOT_EMPTY;
}

// ── Rename ───────────────────────────────────────────────────────────────────

NTSTATUS SftpFileSystem::Rename(
    PVOID FileNode,
    PVOID FileDesc,
    PWSTR FileName,
    PWSTR NewFileName,
    BOOLEAN ReplaceIfExists)
{
    std::string oldPath = ToRemotePath(FileName);
    std::string newPath = ToRemotePath(NewFileName);

    std::lock_guard<std::mutex> lock(_mutex);

    if (ReplaceIfExists) {
        // Check if destination exists and remove it first.
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        if (libssh2_sftp_stat(_sftp, newPath.c_str(), &attrs) == 0) {
            bool destIsDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                             ? (S_ISDIR(attrs.permissions) != 0)
                             : false;
            if (destIsDir)
                libssh2_sftp_rmdir(_sftp, newPath.c_str());
            else
                libssh2_sftp_unlink(_sftp, newPath.c_str());
        }
    }

    unsigned long renameFlags = LIBSSH2_SFTP_RENAME_OVERWRITE
                              | LIBSSH2_SFTP_RENAME_ATOMIC
                              | LIBSSH2_SFTP_RENAME_NATIVE;
    if (libssh2_sftp_rename_ex(_sftp,
                               oldPath.c_str(), static_cast<unsigned int>(oldPath.size()),
                               newPath.c_str(), static_cast<unsigned int>(newPath.size()),
                               renameFlags) != 0)
        return SftpError();

    return STATUS_SUCCESS;
}

// ── GetSecurity / SetSecurity ─────────────────────────────────────────────────

NTSTATUS SftpFileSystem::GetSecurity(
    PVOID FileNode,
    PVOID FileDesc,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T *PSecurityDescriptorSize)
{
    // No security descriptor: WinFSP will synthesise a permissive one.
    if (PSecurityDescriptorSize)
        *PSecurityDescriptorSize = 0;
    return STATUS_SUCCESS;
}

NTSTATUS SftpFileSystem::SetSecurity(
    PVOID FileNode,
    PVOID FileDesc,
    SECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    // Security attributes are not supported over SFTP; silently accept.
    return STATUS_SUCCESS;
}

// ── ReadDirectory ────────────────────────────────────────────────────────────

bool SftpFileSystem::LoadDirEntries(SftpContext *ctx)
{
    // Caller holds _mutex.
    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(_sftp, ctx->remotePathA.c_str());
    if (!dh)
        return false;

    ctx->dirEntries.clear();

    char nameBuf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    for (;;) {
        int rc = libssh2_sftp_readdir(dh, nameBuf, sizeof(nameBuf), &attrs);
        if (rc <= 0) break;

        DirEntry de;
        de.name = Utf8ToWide(nameBuf);
        bool entryIsDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                          ? (S_ISDIR(attrs.permissions) != 0)
                          : false;
        FillFileInfo(&de.info, attrs, entryIsDir);
        ctx->dirEntries.push_back(std::move(de));
    }

    libssh2_sftp_close(dh);

    // Sort case-insensitively for consistent Windows Explorer ordering.
    std::sort(ctx->dirEntries.begin(), ctx->dirEntries.end(),
              [](const DirEntry &a, const DirEntry &b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    ctx->dirLoaded = true;
    return true;
}

BOOLEAN SftpFileSystem::ReadDirectory(
    PVOID FileNode,
    PVOID FileDesc,
    PWSTR Pattern,
    PWSTR Marker,
    PVOID Buffer,
    ULONG Length,
    PULONG PBytesTransferred)
{
    auto *ctx = static_cast<SftpContext *>(FileDesc);

    // Build the listing once per open directory handle.
    if (!ctx->dirLoaded) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!ctx->dirLoaded) { // double-check after acquiring the lock
            if (!LoadDirEntries(ctx))
                return FALSE;
        }
    }

    // Find the starting index: entries strictly after Marker (case-insensitive).
    size_t startIdx = 0;
    if (Marker && *Marker) {
        startIdx = ctx->dirEntries.size(); // default: marker not found — skip all
        for (size_t i = 0; i < ctx->dirEntries.size(); ++i) {
            if (_wcsicmp(ctx->dirEntries[i].name.c_str(), Marker) > 0) {
                startIdx = i;
                break;
            }
        }
    }

    // Build a local FSP_FSCTL_DIR_INFO stack buffer large enough for max component.
    UINT8 dirInfoBuf[sizeof(FSP_FSCTL_DIR_INFO) + 255 * sizeof(WCHAR)];
    auto *dirInfo = reinterpret_cast<FSP_FSCTL_DIR_INFO *>(dirInfoBuf);

    for (size_t i = startIdx; i < ctx->dirEntries.size(); ++i) {
        const DirEntry &de = ctx->dirEntries[i];
        ULONG nameLen = static_cast<ULONG>(de.name.size() * sizeof(WCHAR));

        memset(dirInfo, 0, sizeof(FSP_FSCTL_DIR_INFO));
        dirInfo->Size     = static_cast<UINT16>(
            FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + nameLen);
        dirInfo->FileInfo = de.info;
        memcpy(dirInfo->FileNameBuf, de.name.c_str(), nameLen);

        if (!FspFileSystemAddDirInfo(dirInfo, Buffer, Length, PBytesTransferred))
            return TRUE; // buffer full — WinFSP will call us again with updated Marker
    }

    // Finalise the buffer with a zero-size sentinel.
    FspFileSystemAddDirInfo(nullptr, Buffer, Length, PBytesTransferred);
    return FALSE; // enumeration complete
}
