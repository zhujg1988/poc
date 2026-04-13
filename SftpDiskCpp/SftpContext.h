#pragma once

// SftpContext.h — per-open-file/directory state stored in WinFSP FileNode/FileDesc slots.
//
// Include order matters on Windows: winsock2 must precede windows.h.
// Callers that need winsock (e.g. main.cpp) must include <winsock2.h> before this header.

// Pull in WinFSP C headers for FSP_FSCTL_FILE_INFO (handles windows.h internally).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winfsp/winfsp.h>

#include <string>
#include <vector>

// ── DirEntry ─────────────────────────────────────────────────────────────────
// Pre-computed directory entry stored in the enumeration cache.
// FileInfo is stored by value to avoid heap allocation per entry.
struct DirEntry {
    std::wstring        name; // wide filename (for WinFSP buffer)
    FSP_FSCTL_FILE_INFO info; // pre-computed file info
};

// ── SftpContext ───────────────────────────────────────────────────────────────
// Forward-declare the libssh2 handle type so this header does not need to
// pull in libssh2 headers (which have their own windows/winsock ordering rules).
struct _LIBSSH2_SFTP_HANDLE; // defined in libssh2_sftp.h as typedef struct _LIBSSH2_SFTP_HANDLE

struct SftpContext {
    std::string          remotePathA;  // UTF-8 path on the SFTP server
    bool                 isDirectory;
    _LIBSSH2_SFTP_HANDLE *handle;     // open file handle (nullptr for directories)
    bool                 readOnly;    // file was opened read-only

    // Directory listing cache (built once per open directory handle).
    bool                    dirLoaded;
    std::vector<DirEntry>   dirEntries;

    SftpContext(std::string path, bool isDir, _LIBSSH2_SFTP_HANDLE *h, bool ro = false)
        : remotePathA(std::move(path))
        , isDirectory(isDir)
        , handle(h)
        , readOnly(ro)
        , dirLoaded(false)
    {}

    // Non-copyable, non-movable (WinFSP keeps a raw pointer to us).
    SftpContext(const SftpContext &) = delete;
    SftpContext &operator=(const SftpContext &) = delete;
};
