#pragma once

// SftpFileSystem.h — WinFSP FileSystem subclass that proxies all I/O to a remote
// SFTP server using libssh2.  One instance per mounted drive.

#include "SftpContext.h"        // pulls in winfsp.h + FSP_FSCTL_FILE_INFO
#include <winfsp/winfsp.hpp>    // Fsp::FileSystem / Fsp::FileSystemHost (C++ wrapper)
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <mutex>
#include <string>
#include <vector>

// Helper: convert LIBSSH2_SFTP_ATTRIBUTES::permissions bit to a directory flag.
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & 0xF000u) == 0x4000u)
#endif

// FILE_DIRECTORY_FILE is a standard NT I/O Manager flag (0x00000001).
// WinFSP's own headers define it; add a guard in case they don't.
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif

class SftpFileSystem : public Fsp::FileSystem
{
public:
    // ── construction ──────────────────────────────────────────────────────
    //
    // SftpFileSystem does NOT own the session/sftp pointers; the caller is
    // responsible for shutting them down after Unmount().
    SftpFileSystem(LIBSSH2_SFTP *sftp, const std::string &rootPath);
    ~SftpFileSystem() override = default;

    // ── Fsp::FileSystem overrides ─────────────────────────────────────────

    NTSTATUS GetVolumeInfo(
        FSP_FSCTL_VOLUME_INFO *VolumeInfo) override;

    NTSTATUS GetSecurityByName(
        PWSTR FileName,
        PUINT32 PFileAttributes,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        SIZE_T *PSecurityDescriptorSize) override;

    NTSTATUS Create(
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
        SIZE_T NormalizedNameSize) override;

    NTSTATUS Open(
        PWSTR FileName,
        UINT32 CreateOptions,
        UINT32 GrantedAccess,
        PVOID *PFileNode,
        PVOID *PFileDesc,
        FSP_FSCTL_FILE_INFO *FileInfo,
        PWSTR NormalizedName,
        SIZE_T NormalizedNameSize) override;

    NTSTATUS Overwrite(
        PVOID FileNode,
        PVOID FileDesc,
        UINT32 FileAttributes,
        BOOLEAN ReplaceFileAttributes,
        UINT64 AllocationSize,
        FSP_FSCTL_FILE_INFO *FileInfo) override;

    VOID Cleanup(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR FileName,
        ULONG Flags) override;

    VOID Close(
        PVOID FileNode,
        PVOID FileDesc) override;

    NTSTATUS Read(
        PVOID FileNode,
        PVOID FileDesc,
        PVOID Buffer,
        UINT64 Offset,
        ULONG Length,
        PULONG PBytesTransferred) override;

    NTSTATUS Write(
        PVOID FileNode,
        PVOID FileDesc,
        PVOID Buffer,
        UINT64 Offset,
        ULONG Length,
        BOOLEAN WriteToEndOfFile,
        BOOLEAN ConstrainedIo,
        PULONG PBytesTransferred,
        FSP_FSCTL_FILE_INFO *FileInfo) override;

    NTSTATUS Flush(
        PVOID FileNode,
        PVOID FileDesc,
        FSP_FSCTL_FILE_INFO *FileInfo) override;

    NTSTATUS GetFileInfo(
        PVOID FileNode,
        PVOID FileDesc,
        FSP_FSCTL_FILE_INFO *FileInfo) override;

    NTSTATUS SetBasicInfo(
        PVOID FileNode,
        PVOID FileDesc,
        UINT32 FileAttributes,
        UINT64 CreationTime,
        UINT64 LastAccessTime,
        UINT64 LastWriteTime,
        UINT64 ChangeTime,
        FSP_FSCTL_FILE_INFO *FileInfo) override;

    NTSTATUS SetFileSize(
        PVOID FileNode,
        PVOID FileDesc,
        UINT64 NewSize,
        BOOLEAN SetAllocationSize,
        FSP_FSCTL_FILE_INFO *FileInfo) override;

    NTSTATUS CanDelete(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR FileName) override;

    NTSTATUS Rename(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR FileName,
        PWSTR NewFileName,
        BOOLEAN ReplaceIfExists) override;

    NTSTATUS GetSecurity(
        PVOID FileNode,
        PVOID FileDesc,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        SIZE_T *PSecurityDescriptorSize) override;

    NTSTATUS SetSecurity(
        PVOID FileNode,
        PVOID FileDesc,
        SECURITY_INFORMATION SecurityInformation,
        PSECURITY_DESCRIPTOR ModificationDescriptor) override;

    BOOLEAN ReadDirectory(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR Pattern,
        PWSTR Marker,
        PVOID Buffer,
        ULONG Length,
        PULONG PBytesTransferred) override;

private:
    // ── state ─────────────────────────────────────────────────────────────

    LIBSSH2_SFTP   *_sftp;
    std::string     _rootPath;  // no trailing slash, UTF-8
    std::mutex      _mutex;     // serialises all libssh2 calls (single session)

    static constexpr UINT64 kTotalSize = 1ULL << 40; // 1 TiB synthetic volume size

    // ── private helpers ───────────────────────────────────────────────────

    // Convert a Windows absolute path (backslash) to a UTF-8 POSIX path on the server.
    std::string ToRemotePath(PCWSTR windowsPath) const;

    // Populate *FileInfo from libssh2 attributes. isDir overrides the permissions check.
    static void FillFileInfo(FSP_FSCTL_FILE_INFO *fi,
                             const LIBSSH2_SFTP_ATTRIBUTES &attrs,
                             bool isDir);

    // Stat a path and populate *FileInfo. Caller must NOT hold _mutex.
    NTSTATUS StatPath(const std::string &path, bool isDir, FSP_FSCTL_FILE_INFO *fi);

    // Stat through an open handle. Caller must NOT hold _mutex.
    NTSTATUS StatHandle(LIBSSH2_SFTP_HANDLE *h, bool isDir, FSP_FSCTL_FILE_INFO *fi);

    // Map the current libssh2 SFTP error code to an NTSTATUS.
    NTSTATUS SftpError() const;

    // Convert a Windows FILETIME (100-ns intervals since 1601) to Unix seconds.
    static unsigned long FileTimeToUnix(UINT64 ft);

    // Convert Unix seconds to a Windows FILETIME value.
    static UINT64 UnixToFileTime(unsigned long ut);

    // Helpers for DirEntry building (called from ReadDirectory).
    bool LoadDirEntries(SftpContext *ctx); // must hold _mutex on entry; returns false on error
};
